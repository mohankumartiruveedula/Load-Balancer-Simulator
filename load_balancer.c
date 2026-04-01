/*
 * load_balancer.c - Central TCP Load Balancer
 *
 * Distributes incoming client connections across worker servers
 * using the Least-Connections algorithm.
 *
 * Worker list is configured at the top of main().
 * Default: 3 workers on ports 9001, 9002, 9003 (localhost).
 *
 * Usage: load_balancer
 */

#include "common.h"

/* ── Global worker table ────────────────────────────────────────── */
static worker_info_t workers[MAX_WORKERS];
static int           num_workers = 0;
static mutex_t       worker_mutex;

/* ── Relay context (one per direction) ──────────────────────────── */
typedef struct {
    sock_t src;
    sock_t dst;
    int    worker_idx;          /* index into workers[]              */
    int    is_client_to_worker; /* 1 = client->worker, 0 = reverse  */
} relay_ctx_t;

/* ── Select worker with least connections ───────────────────────── */
static int select_worker(void) {
    int best, i;
    MUTEX_LOCK(worker_mutex);

    if (num_workers == 0) {
        MUTEX_UNLOCK(worker_mutex);
        return -1;
    }

    best = 0;
    for (i = 1; i < num_workers; i++) {
        if (workers[i].active_conns < workers[best].active_conns)
            best = i;
    }

    workers[best].active_conns++;
    workers[best].total_served++;
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

/* ── Print worker status dashboard ──────────────────────────────── */
static void print_dashboard(void) {
    int i;
    MUTEX_LOCK(worker_mutex);
    printf("\n+----------- Worker Dashboard -----------+\n");
    printf("|  #  |    Address        | Active | Total |\n");
    printf("+-----+-------------------+--------+-------+\n");
    for (i = 0; i < num_workers; i++) {
        printf("|  %d  | %s:%-5d     |  %4d  | %5d |\n",
               i, workers[i].ip, workers[i].port,
               workers[i].active_conns, workers[i].total_served);
    }
    printf("+-----+-------------------+--------+-------+\n\n");
    fflush(stdout);
    MUTEX_UNLOCK(worker_mutex);
}

/* ── Connect to a worker server ─────────────────────────────────── */
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

    return fd;
}

/* ── Relay thread: forward data from src -> dst ─────────────────── */
#ifdef _WIN32
static DWORD WINAPI relay_thread(LPVOID arg) {
#else
static void *relay_thread(void *arg) {
#endif
    relay_ctx_t *ctx = (relay_ctx_t *)arg;
    char buf[BUF_SIZE];
    int n;

    while ((n = recv(ctx->src, buf, sizeof(buf), 0)) > 0) {
        int sent = 0;
        while (sent < n) {
            int s = send(ctx->dst, buf + sent, n - sent, 0);
            if (s <= 0) goto done;
            sent += s;
        }
    }

done:
    /* Shutdown both directions to unblock the sibling relay thread */
    shutdown(ctx->src, 1);   /* SD_SEND */
    shutdown(ctx->dst, 1);

    /* Only the client->worker relay does the cleanup */
    if (ctx->is_client_to_worker) {
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
        CLOSE_SOCKET(ctx->src);  /* client fd */
        CLOSE_SOCKET(ctx->dst);  /* worker fd */
        release_worker(ctx->worker_idx);

        LOG("LB", "Session ended for worker %s:%d",
            workers[ctx->worker_idx].ip, workers[ctx->worker_idx].port);
        print_dashboard();
    }

    free(ctx);
    return 0;
}

/* ── Handle a new client connection ─────────────────────────────── */
static void handle_new_client(sock_t client_fd, struct sockaddr_in *client_addr) {
    char client_ip[64];
    int client_port;
    int idx;
    sock_t worker_fd;
    relay_ctx_t *c2w, *w2c;
    thread_t t1, t2;

    INET_NTOP(AF_INET, &client_addr->sin_addr, client_ip, sizeof(client_ip));
    client_port = ntohs(client_addr->sin_port);

    LOG("LB", "New connection from %s:%d", client_ip, client_port);

    /* Select best worker */
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

    /* Connect to the selected worker */
    worker_fd = connect_to_worker(idx);
    if (worker_fd == INVALID_SOCK) {
        LOG("LB", "ERROR: Cannot connect to worker %s:%d",
            workers[idx].ip, workers[idx].port);
        release_worker(idx);
        {
            const char *msg = "ERROR: Worker unavailable.\n";
            send(client_fd, msg, (int)strlen(msg), 0);
        }
        CLOSE_SOCKET(client_fd);
        return;
    }

    print_dashboard();

    /* Create relay context: client -> worker */
    c2w = (relay_ctx_t *)malloc(sizeof(relay_ctx_t));
    c2w->src = client_fd;
    c2w->dst = worker_fd;
    c2w->worker_idx = idx;
    c2w->is_client_to_worker = 1;

    /* Create relay context: worker -> client */
    w2c = (relay_ctx_t *)malloc(sizeof(relay_ctx_t));
    w2c->src = worker_fd;
    w2c->dst = client_fd;
    w2c->worker_idx = idx;
    w2c->is_client_to_worker = 0;

    /* Spawn two relay threads */
    if (thread_create(&t1, relay_thread, c2w) != 0) {
        LOG("LB", "ERROR: Thread creation failed (c2w).");
        free(c2w); free(w2c);
        CLOSE_SOCKET(client_fd);
        CLOSE_SOCKET(worker_fd);
        release_worker(idx);
        return;
    }
    thread_detach(t1);

    if (thread_create(&t2, relay_thread, w2c) != 0) {
        LOG("LB", "ERROR: Thread creation failed (w2c).");
        free(w2c);
        return;
    }
    thread_detach(t2);
}

/* ── Main ───────────────────────────────────────────────────────── */
int main(void) {
    sock_t server_fd;
    int opt = 1;
    int i;
    struct sockaddr_in addr;

    if (sock_init() != 0) {
        fprintf(stderr, "Error: Socket initialization failed.\n");
        return 1;
    }

    MUTEX_INIT(worker_mutex);

    /* ── Configure workers ──────────────────────────────────────── */
    num_workers = 3;

    strcpy(workers[0].ip, "127.0.0.1");
    workers[0].port         = 9001;
    workers[0].active_conns = 0;
    workers[0].total_served = 0;

    strcpy(workers[1].ip, "127.0.0.1");
    workers[1].port         = 9002;
    workers[1].active_conns = 0;
    workers[1].total_served = 0;

    strcpy(workers[2].ip, "127.0.0.1");
    workers[2].port         = 9003;
    workers[2].active_conns = 0;
    workers[2].total_served = 0;

    /* ── Create listening socket ────────────────────────────────── */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCK) {
        fprintf(stderr, "Error: socket() failed (%d).\n", sock_errno());
        sock_cleanup();
        return 1;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(LB_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCK_ERR) {
        fprintf(stderr, "Error: bind() failed on port %d (%d).\n", LB_PORT, sock_errno());
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
    printf("     TCP LOAD BALANCER (Least Connections Algorithm)\n");
    printf("     Listening on port %d\n", LB_PORT);
    printf("========================================================\n");
    printf("  Workers:\n");
    for (i = 0; i < num_workers; i++) {
        printf("    [%d] %s:%d\n", i, workers[i].ip, workers[i].port);
    }
    printf("========================================================\n\n");

    /* ── Accept loop ────────────────────────────────────────────── */
    for (;;) {
        struct sockaddr_in client_addr;
        int addrlen = sizeof(client_addr);
        sock_t client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);
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
