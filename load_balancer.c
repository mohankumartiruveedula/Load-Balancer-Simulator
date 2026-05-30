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
#include <stdarg.h>
#include <stdint.h>
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

/* ── Global dashboard & SSE configuration ────────────────────────── */
static sock_t sse_clients[MAX_SSE_CLIENTS];
static int    num_sse_clients = 0;
static mutex_t sse_mutex;
static char  *dashboard_html = NULL;
static int    dashboard_html_len = 0;

#ifdef _WIN32
#define SEND_FLAGS 0
#else
#define SEND_FLAGS MSG_NOSIGNAL
#endif

static void load_dashboard_html(void) {
    FILE *f = fopen("dashboard.html", "rb");
    if (!f) {
        LOG("LB", "WARNING: dashboard.html not found, using embedded minimal dashboard");
        const char *fallback = "<html><head><title>Dashboard Error</title></head><body><h1>Dashboard HTML file not found!</h1><p>Place dashboard.html in execution directory.</p></body></html>";
        dashboard_html = malloc(strlen(fallback) + 1);
        if (dashboard_html) {
            strcpy(dashboard_html, fallback);
            dashboard_html_len = (int)strlen(fallback);
        }
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    dashboard_html = malloc(size + 1);
    if (dashboard_html) {
        size_t read_bytes = fread(dashboard_html, 1, size, f);
        dashboard_html[read_bytes] = '\0';
        dashboard_html_len = (int)read_bytes;
        LOG("LB", "Loaded dashboard.html (%d bytes)", (int)read_bytes);
    }
    fclose(f);
}

static void broadcast_sse(const char *event, const char *data) {
    char buf[BUF_SIZE * 2];
    int len;
    if (event) {
        len = snprintf(buf, sizeof(buf), "event: %s\ndata: %s\n\n", event, data);
    } else {
        len = snprintf(buf, sizeof(buf), "data: %s\n\n", data);
    }

    MUTEX_LOCK(sse_mutex);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (sse_clients[i] != INVALID_SOCK) {
            int sent = send(sse_clients[i], buf, len, SEND_FLAGS);
            if (sent <= 0) {
                LOG("LB", "Removing disconnected SSE client %d", (int)sse_clients[i]);
                CLOSE_SOCKET(sse_clients[i]);
                sse_clients[i] = INVALID_SOCK;
                if (num_sse_clients > 0) num_sse_clients--;
            }
        }
    }
    MUTEX_UNLOCK(sse_mutex);
}

static void broadcast_state(void) {
    worker_info_t local_workers[MAX_WORKERS];
    int local_num_workers = 0;
    int total_served = 0;
    int active_sessions = 0;

    MUTEX_LOCK(worker_mutex);
    local_num_workers = num_workers;
    for (int i = 0; i < num_workers; i++) {
        local_workers[i] = workers[i];
    }
    MUTEX_UNLOCK(worker_mutex);

    char json[2048];
    int offset = 0;
    offset += snprintf(json + offset, sizeof(json) - offset, "{\"workers\":[");
    for (int i = 0; i < local_num_workers; i++) {
        offset += snprintf(json + offset, sizeof(json) - offset,
            "%s{\"id\":%d,\"ip\":\"%s\",\"port\":%d,\"active_conns\":%d,\"total_served\":%d,\"is_alive\":%d}",
            (i > 0 ? "," : ""),
            i, local_workers[i].ip, local_workers[i].port,
            local_workers[i].active_conns, local_workers[i].total_served,
            local_workers[i].is_alive);
        total_served += local_workers[i].total_served;
        active_sessions += local_workers[i].active_conns;
    }
    offset += snprintf(json + offset, sizeof(json) - offset, "],\"total_served\":%d,\"active_sessions\":%d}",
                        total_served, active_sessions);

    broadcast_sse("state", json);
}

static void broadcast_log(const char *tag, const char *fmt, ...) {
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char ts[20];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm_info);

    char escaped_msg[2048];
    int j = 0;
    for (int i = 0; message[i] != '\0' && j < (int)sizeof(escaped_msg) - 2; i++) {
        if (message[i] == '"' || message[i] == '\\') {
            escaped_msg[j++] = '\\';
        }
        escaped_msg[j++] = message[i];
    }
    escaped_msg[j] = '\0';

    char json[3000];
    snprintf(json, sizeof(json), "{\"timestamp\":\"%s\",\"tag\":\"%s\",\"message\":\"%s\"}",
             ts, tag, escaped_msg);
    
    broadcast_sse("log", json);
}

static void handle_http_request(sock_t client_fd) {
    char buf[2048];
    int received = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (received <= 0) {
        CLOSE_SOCKET(client_fd);
        return;
    }
    buf[received] = '\0';

    char method[16] = {0};
    char path[256] = {0};
    if (sscanf(buf, "%15s %255s", method, path) < 2) {
        CLOSE_SOCKET(client_fd);
        return;
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0) {
            char headers[512];
            int len = snprintf(headers, sizeof(headers),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n",
                dashboard_html_len);
            send(client_fd, headers, len, 0);
            send(client_fd, dashboard_html, dashboard_html_len, 0);
            CLOSE_SOCKET(client_fd);
        }
        else if (strcmp(path, "/events") == 0) {
            const char *headers =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n"
                "Access-Control-Allow-Origin: *\r\n\r\n";
            send(client_fd, headers, (int)strlen(headers), 0);

            MUTEX_LOCK(sse_mutex);
            int added = 0;
            for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
                if (sse_clients[i] == INVALID_SOCK) {
                    sse_clients[i] = client_fd;
                    num_sse_clients++;
                    added = 1;
                    break;
                }
            }
            MUTEX_UNLOCK(sse_mutex);

            if (!added) {
                LOG("LB", "WARNING: Max SSE clients reached. Rejecting client.");
                CLOSE_SOCKET(client_fd);
            } else {
                LOG("LB", "New SSE subscriber from client fd %d (total: %d)", (int)client_fd, num_sse_clients);
                broadcast_state();
            }
        }
        else if (strcmp(path, "/api/status") == 0) {
            worker_info_t local_workers[MAX_WORKERS];
            int local_num_workers = 0;
            int total_served = 0;
            int active_sessions = 0;

            MUTEX_LOCK(worker_mutex);
            local_num_workers = num_workers;
            for (int i = 0; i < num_workers; i++) {
                local_workers[i] = workers[i];
            }
            MUTEX_UNLOCK(worker_mutex);

            char json[2048];
            int offset = 0;
            offset += snprintf(json + offset, sizeof(json) - offset, "{\"workers\":[");
            for (int i = 0; i < local_num_workers; i++) {
                offset += snprintf(json + offset, sizeof(json) - offset,
                    "%s{\"id\":%d,\"ip\":\"%s\",\"port\":%d,\"active_conns\":%d,\"total_served\":%d,\"is_alive\":%d}",
                    (i > 0 ? "," : ""),
                    i, local_workers[i].ip, local_workers[i].port,
                    local_workers[i].active_conns, local_workers[i].total_served,
                    local_workers[i].is_alive);
                total_served += local_workers[i].total_served;
                active_sessions += local_workers[i].active_conns;
            }
            offset += snprintf(json + offset, sizeof(json) - offset, "],\"total_served\":%d,\"active_sessions\":%d}",
                                total_served, active_sessions);

            char headers[512];
            int len = snprintf(headers, sizeof(headers),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n"
                "Access-Control-Allow-Origin: *\r\n\r\n",
                (int)strlen(json));
            send(client_fd, headers, len, 0);
            send(client_fd, json, (int)strlen(json), 0);
            CLOSE_SOCKET(client_fd);
        }
        else {
            const char *res = 
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 9\r\n"
                "Connection: close\r\n\r\n"
                "Not Found";
            send(client_fd, res, (int)strlen(res), 0);
            CLOSE_SOCKET(client_fd);
        }
    } else {
        const char *res = 
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Length: 18\r\n"
            "Connection: close\r\n\r\n"
            "Method Not Allowed";
        send(client_fd, res, (int)strlen(res), 0);
        CLOSE_SOCKET(client_fd);
    }
}

#ifdef _WIN32
static DWORD WINAPI http_client_handler(LPVOID arg) {
#else
static void *http_client_handler(void *arg) {
#endif
    sock_t client_fd = (sock_t)(uintptr_t)arg;
    handle_http_request(client_fd);
    return 0;
}

#ifdef _WIN32
static DWORD WINAPI http_server_thread(LPVOID arg) {
#else
static void *http_server_thread(void *arg) {
#endif
    (void)arg;
    sock_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCK) {
        LOG("LB-HTTP", "ERROR: Could not create socket (%d)", sock_errno());
        return 0;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(DASHBOARD_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCK_ERR) {
        LOG("LB-HTTP", "ERROR: bind failed on port %d (%d)", DASHBOARD_PORT, sock_errno());
        CLOSE_SOCKET(server_fd);
        return 0;
    }

    if (listen(server_fd, 10) == SOCK_ERR) {
        LOG("LB-HTTP", "ERROR: listen failed (%d)", sock_errno());
        CLOSE_SOCKET(server_fd);
        return 0;
    }

    LOG("LB", "Dashboard HTTP server listening on port %d", DASHBOARD_PORT);

    for (;;) {
        struct sockaddr_in client_addr;
        int addrlen = sizeof(client_addr);
        sock_t client_fd;
#ifdef _WIN32
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);
#else
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &(unsigned int)addrlen);
#endif
        if (client_fd == INVALID_SOCK) {
            continue;
        }

        thread_t t;
        if (thread_create(&t, http_client_handler, (void *)(uintptr_t)client_fd) != 0) {
            CLOSE_SOCKET(client_fd);
        } else {
            thread_detach(t);
        }
    }

    CLOSE_SOCKET(server_fd);
    return 0;
}

/* ── Select worker with least connections ───────────────────────── */
/*  Pass 1: prefer alive workers (least-connections).                  */
/*  Pass 2: if none alive, retry dead workers past WORKER_RETRY_SECS   */
/*           by tentatively re-marking them alive; connect_to_worker    */
/*           confirms the real state (marks dead again on failure).    */
#define WORKER_RETRY_SECS 15

static int select_worker(void) {
    int    best = -1, i;
    time_t now  = time(NULL);
    int    healed_indices[MAX_WORKERS];
    int    healed_count = 0;
    int    retried_idx = -1;

    MUTEX_LOCK(worker_mutex);

    /* Pass 0: heal DOWN workers that are now reachable */
    for (i = 0; i < num_workers; i++) {
        if (workers[i].is_alive) continue;
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
                healed_indices[healed_count++] = i;
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
                retried_idx = i;
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

    /* Perform all logging and broadcasting outside the critical section */
    for (i = 0; i < healed_count; i++) {
        int idx = healed_indices[i];
        LOG("LB", "Worker %s:%d is back ALIVE (proactive check)",
            workers[idx].ip, workers[idx].port);
        broadcast_log("LB", "Worker %s:%d is back ALIVE (proactive check)",
                      workers[idx].ip, workers[idx].port);
    }

    if (retried_idx >= 0) {
        LOG("LB", "Retrying previously-dead worker %s:%d",
            workers[retried_idx].ip, workers[retried_idx].port);
        broadcast_log("LB", "Retrying previously-dead worker %s:%d",
                      workers[retried_idx].ip, workers[retried_idx].port);
    }

    broadcast_state();
    return best;
}

/* ── Decrement worker connection count ──────────────────────────── */
static void release_worker(int idx) {
    MUTEX_LOCK(worker_mutex);
    if (workers[idx].active_conns > 0)
        workers[idx].active_conns--;
    MUTEX_UNLOCK(worker_mutex);
    broadcast_state();
}

/* ── Mark a worker as dead, record timestamp ────────────────── */
/* Every session that fails over calls this independently, so we       */
/* always decrement active_conns (not guarded by is_alive), and only   */
/* log + record dead_since the first time is_alive flips.              */
static void mark_worker_dead(int idx) {
    int marked_dead = 0;
    MUTEX_LOCK(worker_mutex);
    if (workers[idx].is_alive) {
        workers[idx].is_alive  = 0;
        workers[idx].dead_since = time(NULL);
        marked_dead = 1;
    }
    /* Always decrement so every failing session releases its slot */
    if (workers[idx].active_conns > 0)
        workers[idx].active_conns--;
    MUTEX_UNLOCK(worker_mutex);

    if (marked_dead) {
        LOG("LB", "Worker %s:%d marked as DOWN",
            workers[idx].ip, workers[idx].port);
        broadcast_log("LB", "Worker %s:%d marked as DOWN",
                      workers[idx].ip, workers[idx].port);
    }
    broadcast_state();
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
    broadcast_state();
}

/* ── Connect to a worker server ─────────────────────────────────── */
/* If the worker was marked dead but the connect succeeds (it came    */
/* back up), we silently restore is_alive = 1.                        */
static sock_t connect_to_worker(int idx) {
    sock_t fd;
    struct sockaddr_in addr;
    int restored = 0;

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
        restored = 1;
    }
    MUTEX_UNLOCK(worker_mutex);

    if (restored) {
        LOG("LB", "Worker %s:%d is back ALIVE",
            workers[idx].ip, workers[idx].port);
        broadcast_log("LB", "Worker %s:%d is back ALIVE",
                      workers[idx].ip, workers[idx].port);
    }
    broadcast_state();

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
            if (send(worker_fd, buf, n, SEND_FLAGS) <= 0)
                goto worker_failed;                /* worker write error   */
        }

        /* ── worker → client ── */
        if (FD_ISSET(worker_fd, &readfds)) {
            n = recv(worker_fd, buf, sizeof(buf), 0);
            if (n < 0)  goto worker_failed;        /* worker error / crash */
            if (n == 0) goto session_end;          /* clean QUIT close     */
            if (send(client_fd, buf, n, SEND_FLAGS) <= 0) goto session_end;
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
            broadcast_log("LB", "Worker %s:%d failed – attempting failover",
                          workers[worker_idx].ip, workers[worker_idx].port);

            new_idx = select_worker();
            if (new_idx < 0) {
                const char *err = "\n[LB] All servers are down. Disconnecting.\n";
                send(client_fd, err, (int)strlen(err), SEND_FLAGS);
                goto session_end;
            }

            new_wfd = connect_to_worker(new_idx);
            if (new_wfd == INVALID_SOCK) {
                release_worker(new_idx);
                const char *err = "\n[LB] Failover failed. Disconnecting.\n";
                send(client_fd, err, (int)strlen(err), SEND_FLAGS);
                goto session_end;
            }

            /* Inform the client – they stay connected, no re-dial needed */
            {
                const char *notice =
                    "\n[LB] Server failed. Reconnecting to another server...\n";
                send(client_fd, notice, (int)strlen(notice), SEND_FLAGS);
            }

            LOG("LB", "Failover successful -> Worker %s:%d",
                workers[new_idx].ip, workers[new_idx].port);
            broadcast_log("LB", "Failover successful -> Worker %s:%d",
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
    broadcast_log("LB", "Session ended (worker %s:%d)",
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
    broadcast_log("LB", "New connection from %s:%d", client_ip, client_port);

    idx = select_worker();
    if (idx < 0) {
        const char *msg = "ERROR: No workers available.\n";
        send(client_fd, msg, (int)strlen(msg), SEND_FLAGS);
        CLOSE_SOCKET(client_fd);
        return;
    }

    LOG("LB", "Routing %s:%d -> Worker %s:%d  (active: %d)",
        client_ip, client_port,
        workers[idx].ip, workers[idx].port,
        workers[idx].active_conns);
    broadcast_log("LB", "Routing %s:%d -> Worker %s:%d  (active: %d)",
                  client_ip, client_port,
                  workers[idx].ip, workers[idx].port,
                  workers[idx].active_conns);

    worker_fd = connect_to_worker(idx);
    if (worker_fd == INVALID_SOCK) {
        LOG("LB", "Cannot connect to worker %s:%d – marking DOWN",
            workers[idx].ip, workers[idx].port);
        broadcast_log("LB", "Cannot connect to worker %s:%d – marking DOWN",
                      workers[idx].ip, workers[idx].port);
        mark_worker_dead(idx);
        /* Immediately try another worker */
        idx = select_worker();
        if (idx >= 0) {
            worker_fd = connect_to_worker(idx);
            if (worker_fd != INVALID_SOCK) {
                LOG("LB", "Routing %s:%d -> Worker %s:%d  (active: %d)",
                    client_ip, client_port,
                    workers[idx].ip, workers[idx].port,
                    workers[idx].active_conns);
                broadcast_log("LB", "Routing %s:%d -> Worker %s:%d  (active: %d)",
                              client_ip, client_port,
                              workers[idx].ip, workers[idx].port,
                              workers[idx].active_conns);
            }
        }
        if (idx < 0 || worker_fd == INVALID_SOCK) {
            if (idx >= 0) release_worker(idx);
            const char *msg = "ERROR: No workers available.\n";
            send(client_fd, msg, (int)strlen(msg), SEND_FLAGS);
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
        broadcast_log("LB", "ERROR: Thread creation failed.");
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
        int port = BASE_WORKER_PORT + i;
#ifdef _WIN32
        STARTUPINFO si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

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
    MUTEX_INIT(sse_mutex);

    for (i = 0; i < MAX_SSE_CLIENTS; i++) {
        sse_clients[i] = INVALID_SOCK;
    }
    num_sse_clients = 0;

    load_dashboard_html();

    /* Start HTTP Server Thread for Dashboard */
    {
        thread_t http_server_tid;
        if (thread_create(&http_server_tid, http_server_thread, NULL) != 0) {
            fprintf(stderr, "Error: Failed to start HTTP server thread.\n");
        } else {
            thread_detach(http_server_tid);
        }
    }

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
        sock_t client_fd;
        #ifdef _WIN32 
            client_fd = accept(server_fd,
                                    (struct sockaddr *)&client_addr,
                                    &addrlen);
        #else
                client_fd = accept(server_fd,
                                    (struct sockaddr *)&client_addr,
                                    &(unsigned int)addrlen);
        #endif
        if (client_fd == INVALID_SOCK) {
            LOG("LB", "accept() failed (%d), continuing...", sock_errno());
            continue;
        }
        handle_new_client(client_fd, &client_addr);
    }

    MUTEX_DESTROY(worker_mutex);
    MUTEX_DESTROY(sse_mutex);
    if (dashboard_html) {
        free(dashboard_html);
    }
    CLOSE_SOCKET(server_fd);
    sock_cleanup();
    return 0;
}
