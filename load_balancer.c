/*
 * load_balancer.c - Central TCP Load Balancer with Worker Failover
 *
 * - Least-Connections scheduling (skips dead workers).
 * - Transparent failover: if a worker crashes mid-session, the client
 *   is seamlessly re-routed to the next healthy worker without
 *   the client needing to reconnect.
 * - Background health-check thread re-marks workers ALIVE when they
 *   come back up.
 *
 * Usage: load_balancer <ip:port> [ip:port ...]
 *   e.g. load_balancer
 */

#include "common.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ── Worker port configuration ───────────────────────────────────── */
#define NUM_WORKER_PORTS  3
#define BASE_WORKER_PORT  9001

/* ── Global worker table ────────────────────────────────────────── */
static worker_info_t workers[MAX_WORKERS];
static int           num_workers = 0;
static mutex_t       worker_mutex;

/* ── Select worker with least connections ───────────────────────── */
/*  Pass 1: prefer alive workers (least-connections).                  */
/*  Pass 2: if none alive, retry dead workers past WORKER_RETRY_SECS   */
/*           by tentatively re-marking them alive; connect_to_worker    */
/*           confirms the real state (marks dead again on failure).    */
#define WORKER_RETRY_SECS 15

static int select_worker(void) {
    int    best = -1, i;
    time_t now  = time(NULL);
    MUTEX_LOCK(worker_mutex);

    /* Pass 0: heal DOWN workers that are now reachable */
    for (i = 0; i < num_workers; i++) {
        if (workers[i].is_alive) continue;
        /* Try a quick connect to see if the worker came back */
        sock_t probe = socket(AF_INET, SOCK_STREAM, 0);
        if (probe != INVALID_SOCK) {
            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port   = htons((unsigned short)workers[i].port);
            INET_PTON(AF_INET, workers[i].ip, &a.sin_addr);
            if (connect(probe, (struct sockaddr *)&a, sizeof(a)) == 0) {
                CLOSE_SOCKET(probe);
                workers[i].is_alive  = 1;
                workers[i].dead_since = 0;
                LOG("LB", "Worker %s:%d is back ALIVE (proactive check)",
                    workers[i].ip, workers[i].port);
            } else {
                CLOSE_SOCKET(probe);
            }
        }
    }

    /* Pass 1: alive workers only */
    for (i = 0; i < num_workers; i++) {
        if (!workers[i].is_alive) continue;
        if (best < 0 || workers[i].active_conns < workers[best].active_conns)
            best = i;
    }

    /* Pass 2: try dead workers that have passed the retry window */
    if (best < 0) {
        for (i = 0; i < num_workers; i++) {
            if (workers[i].is_alive) continue;
            if (now - workers[i].dead_since >= WORKER_RETRY_SECS) {
                workers[i].is_alive = 1; /* tentative: confirmed by connect */
                LOG("LB", "Retrying previously-dead worker %s:%d",
                    workers[i].ip, workers[i].port);
                best = i;
                break;
            }
        }
    }

    if (best >= 0) {
        workers[best].active_conns++;
        workers[best].total_served++;
    }
    MUTEX_UNLOCK(worker_mutex);
    return best;
}

/* ── Decrement worker connection count ──────────────────────────── */
static void release_worker(int idx) {
    MUTEX_LOCK(worker_mutex);
    if (workers[idx].active_conns > 0)
        workers[idx].active_conns--;
    MUTEX_UNLOCK(worker_mutex);
}

/* ── Mark a worker as dead, record timestamp ────────────────── */
/* Every session that fails over calls this independently, so we       */
/* always decrement active_conns (not guarded by is_alive), and only   */
/* log + record dead_since the first time is_alive flips.              */
static void mark_worker_dead(int idx) {
    MUTEX_LOCK(worker_mutex);
    if (workers[idx].is_alive) {
        workers[idx].is_alive  = 0;
        workers[idx].dead_since = time(NULL);
        LOG("LB", "Worker %s:%d marked as DOWN",
            workers[idx].ip, workers[idx].port);
    }
    /* Always decrement so every failing session releases its slot */
    if (workers[idx].active_conns > 0)
        workers[idx].active_conns--;
    MUTEX_UNLOCK(worker_mutex);
}

/* ── Print worker status dashboard ──────────────────────────────── */
static void print_dashboard(void) {
    int i;
    MUTEX_LOCK(worker_mutex);
    printf("\n+----------- Worker Dashboard -----------+\n");
    printf("|  #  |    Address        | Active | Status |\n");
    printf("+-----+-------------------+--------+--------+\n");
    for (i = 0; i < num_workers; i++) {
        printf("|  %d  | %s:%-5d     |  %4d  | %-5s  |\n",
               i, workers[i].ip, workers[i].port,
               workers[i].active_conns,
               workers[i].is_alive ? "ALIVE" : "DOWN");
    }
    printf("+-----+-------------------+--------+--------+\n\n");
    fflush(stdout);
    MUTEX_UNLOCK(worker_mutex);
}

/* ── Connect to a worker server ─────────────────────────────────── */
/* If the worker was marked dead but the connect succeeds (it came    */
/* back up), we silently restore is_alive = 1.                        */
static sock_t connect_to_worker(int idx) {
    sock_t fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK) return INVALID_SOCK;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)workers[idx].port);

    if (INET_PTON(AF_INET, workers[idx].ip, &addr.sin_addr) <= 0) {
        CLOSE_SOCKET(fd);
        return INVALID_SOCK;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCK_ERR) {
        CLOSE_SOCKET(fd);
        return INVALID_SOCK;
    }

    /* Worker is reachable – restore ALIVE if it was previously dead */
    MUTEX_LOCK(worker_mutex);
    if (!workers[idx].is_alive) {
        workers[idx].is_alive = 1;
        LOG("LB", "Worker %s:%d is back ALIVE",
            workers[idx].ip, workers[idx].port);
    }
    MUTEX_UNLOCK(worker_mutex);

    return fd;
}

/* ── Session context passed to the relay thread ─────────────────── */
typedef struct {
    sock_t client_fd;
    sock_t worker_fd;
    int    worker_idx;
} session_ctx_t;

/*
 * ── Single bidirectional relay thread with inline failover ────────
 *
 * Uses select() to multiplex client_fd and worker_fd in one thread,
 * eliminating any race condition when we need to swap the worker
 * socket during a failover.
 *
 * Failover trigger:
 *   recv(worker_fd) < 0   → worker crashed (RST / error) → failover
 *   send(worker_fd) error → write to worker failed        → failover
 *
 * Normal close (no failover):
 *   recv(worker_fd) == 0  → worker did a clean FIN (QUIT sequence)
 *   recv(client_fd) <= 0  → client disconnected
 */
#ifdef _WIN32
static DWORD WINAPI relay_session(LPVOID arg) {
#else
static void *relay_session(void *arg) {
#endif
    session_ctx_t *ctx        = (session_ctx_t *)arg;
    sock_t         client_fd  = ctx->client_fd;
    sock_t         worker_fd  = ctx->worker_fd;
    int            worker_idx = ctx->worker_idx;
    char           buf[BUF_SIZE];
    int            n;
    free(ctx);

    for (;;) {
        fd_set readfds;
        int    ret;

        FD_ZERO(&readfds);
        FD_SET(client_fd, &readfds);
        FD_SET(worker_fd, &readfds);

#ifdef _WIN32
        ret = select(0, &readfds, NULL, NULL, NULL); /* nfds ignored on Win */
#else
        {
            int maxfd = (client_fd > worker_fd ?
                         (int)client_fd : (int)worker_fd) + 1;
            ret = select(maxfd, &readfds, NULL, NULL, NULL);
        }
#endif
        if (ret <= 0) break;

        /* ── client → worker ── */
        if (FD_ISSET(client_fd, &readfds)) {
            n = recv(client_fd, buf, sizeof(buf), 0);
            if (n <= 0) goto session_end;          /* client disconnected  */
            if (send(worker_fd, buf, n, 0) <= 0)
                goto worker_failed;                /* worker write error   */
        }

        /* ── worker → client ── */
        if (FD_ISSET(worker_fd, &readfds)) {
            n = recv(worker_fd, buf, sizeof(buf), 0);
            if (n < 0)  goto worker_failed;        /* worker error / crash */
            if (n == 0) goto session_end;          /* clean QUIT close     */
            if (send(client_fd, buf, n, 0) <= 0) goto session_end;
        }
        continue;

    /* ── Worker failure: attempt transparent failover ─────────────── */
    worker_failed: {
            int    new_idx;
            sock_t new_wfd;

            CLOSE_SOCKET(worker_fd);
            mark_worker_dead(worker_idx);
            LOG("LB", "Worker %s:%d failed – attempting failover",
                workers[worker_idx].ip, workers[worker_idx].port);

            new_idx = select_worker();
            if (new_idx < 0) {
                const char *err = "\n[LB] All servers are down. Disconnecting.\n";
                send(client_fd, err, (int)strlen(err), 0);
                goto session_end;
            }

            new_wfd = connect_to_worker(new_idx);
            if (new_wfd == INVALID_SOCK) {
                release_worker(new_idx);
                const char *err = "\n[LB] Failover failed. Disconnecting.\n";
                send(client_fd, err, (int)strlen(err), 0);
                goto session_end;
            }

            /* Inform the client – they stay connected, no re-dial needed */
            {
                const char *notice =
                    "\n[LB] Server failed. Reconnecting to another server...\n";
                send(client_fd, notice, (int)strlen(notice), 0);
            }

            LOG("LB", "Failover successful -> Worker %s:%d",
                workers[new_idx].ip, workers[new_idx].port);

            worker_fd  = new_wfd;
            worker_idx = new_idx;
            print_dashboard();
            continue;   /* resume the select() loop with the new worker */
        }
    }

session_end:
    CLOSE_SOCKET(client_fd);
    CLOSE_SOCKET(worker_fd);
    release_worker(worker_idx);
    LOG("LB", "Session ended (worker %s:%d)",
        workers[worker_idx].ip, workers[worker_idx].port);
    print_dashboard();
    return 0;
}

/* ── Handle a new client connection ─────────────────────────────── */
static void handle_new_client(sock_t client_fd,
                               struct sockaddr_in *client_addr) {
    char           client_ip[64];
    int            client_port;
    int            idx;
    sock_t         worker_fd;
    session_ctx_t *ctx;
    thread_t       t;

    INET_NTOP(AF_INET, &client_addr->sin_addr, client_ip, sizeof(client_ip));
    client_port = ntohs(client_addr->sin_port);

    LOG("LB", "New connection from %s:%d", client_ip, client_port);

    idx = select_worker();
    if (idx < 0) {
        const char *msg = "ERROR: No workers available.\n";
        send(client_fd, msg, (int)strlen(msg), 0);
        CLOSE_SOCKET(client_fd);
        return;
    }

    LOG("LB", "Routing %s:%d -> Worker %s:%d  (active: %d)",
        client_ip, client_port,
        workers[idx].ip, workers[idx].port,
        workers[idx].active_conns);

    worker_fd = connect_to_worker(idx);
    if (worker_fd == INVALID_SOCK) {
        LOG("LB", "Cannot connect to worker %s:%d – marking DOWN",
            workers[idx].ip, workers[idx].port);
        mark_worker_dead(idx);
        /* Immediately try another worker */
        idx = select_worker();
        if (idx >= 0) worker_fd = connect_to_worker(idx);
        if (idx < 0 || worker_fd == INVALID_SOCK) {
            if (idx >= 0) release_worker(idx);
            const char *msg = "ERROR: No workers available.\n";
            send(client_fd, msg, (int)strlen(msg), 0);
            CLOSE_SOCKET(client_fd);
            return;
        }
    }

    ctx = (session_ctx_t *)malloc(sizeof(session_ctx_t));
    if (!ctx) {
        CLOSE_SOCKET(client_fd);
        CLOSE_SOCKET(worker_fd);
        release_worker(idx);
        return;
    }
    ctx->client_fd  = client_fd;
    ctx->worker_fd  = worker_fd;
    ctx->worker_idx = idx;

    print_dashboard();

    if (thread_create(&t, relay_session, ctx) != 0) {
        LOG("LB", "ERROR: Thread creation failed.");
        free(ctx);
        CLOSE_SOCKET(client_fd);
        CLOSE_SOCKET(worker_fd);
        release_worker(idx);
        return;
    }
    thread_detach(t);
}

/* ── Spawn Worker Processes ─────────────────────────────────────── */
static void spawn_workers(void) {
    int i;
    for (i = 0; i < NUM_WORKER_PORTS; i++) {
        char cmd[256];
#ifdef _WIN32
        STARTUPINFO si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        int port = BASE_WORKER_PORT + i;
        snprintf(cmd, sizeof(cmd), "worker_server.exe %d", port);
        if (CreateProcess(NULL, cmd, NULL, NULL, FALSE,
                          CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            LOG("LB", "Spawned worker on port %d (PID %lu)",
                port, (unsigned long)pi.dwProcessId);
        } else {
            LOG("LB", "WARNING: Failed to spawn worker on port %d (err %lu)",
                port, (unsigned long)GetLastError());
        }
#else
        pid_t pid = fork();
        if (pid == 0) {
            snprintf(cmd, sizeof(cmd), "%d", port);
            execl("./worker_server", "worker_server", cmd, NULL);
            perror("execl");
            _exit(1);
        } else if (pid > 0) {
            LOG("LB", "Spawned worker on port %d (PID %d)", port, pid);
        }
#endif
        /* Brief pause so the worker socket is ready before LB begins accepting */
#ifdef _WIN32
        Sleep(300);
#else
        usleep(300000);
#endif
    }
}

/* ── Main ───────────────────────────────────────────────────────── */
int main(void) {
    sock_t             server_fd;
    int                opt = 1;
    int                i;
    struct sockaddr_in addr;

    if (sock_init() != 0) {
        fprintf(stderr, "Error: Socket initialization failed.\n");
        return 1;
    }

    MUTEX_INIT(worker_mutex);

    num_workers = 0;
    for (i = 0; i < NUM_WORKER_PORTS; i++) {
        if (num_workers >= MAX_WORKERS) break;
        strncpy(workers[num_workers].ip, "127.0.0.1", sizeof(workers[num_workers].ip) - 1);
        workers[num_workers].ip[sizeof(workers[num_workers].ip) - 1] = '\0';
        workers[num_workers].port          = BASE_WORKER_PORT + i;
        workers[num_workers].active_conns  = 0;
        workers[num_workers].total_served  = 0;
        workers[num_workers].is_alive      = 1;   /* assumed alive at start */
        workers[num_workers].dead_since    = 0;
        num_workers++;
    }

    spawn_workers();

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCK) {
        fprintf(stderr, "Error: socket() failed (%d).\n", sock_errno());
        sock_cleanup();
        return 1;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(LB_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCK_ERR) {
        fprintf(stderr, "Error: bind() failed on port %d (%d).\n",
                LB_PORT, sock_errno());
        CLOSE_SOCKET(server_fd);
        sock_cleanup();
        return 1;
    }

    if (listen(server_fd, MAX_PENDING) == SOCK_ERR) {
        fprintf(stderr, "Error: listen() failed (%d).\n", sock_errno());
        CLOSE_SOCKET(server_fd);
        sock_cleanup();
        return 1;
    }

    printf("========================================================\n");
    printf("  TCP LOAD BALANCER (Least Connections + Failover)\n");
    printf("  Listening on port %d\n", LB_PORT);
    printf("========================================================\n");
    printf("  Workers:\n");
    for (i = 0; i < num_workers; i++)
        printf("    [%d] %s:%d\n", i, workers[i].ip, workers[i].port);
    printf("========================================================\n\n");

    /* ── Accept loop ─────────────────────────────────────────────── */
    for (;;) {
        struct sockaddr_in client_addr;
        int    addrlen  = sizeof(client_addr);
        sock_t client_fd = accept(server_fd,
                                  (struct sockaddr *)&client_addr,
                                  &addrlen);
        if (client_fd == INVALID_SOCK) {
            LOG("LB", "accept() failed (%d), continuing...", sock_errno());
            continue;
        }
        handle_new_client(client_fd, &client_addr);
    }

    MUTEX_DESTROY(worker_mutex);
    CLOSE_SOCKET(server_fd);
    sock_cleanup();
    return 0;
}
