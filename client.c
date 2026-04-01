/*
 * client.c - Interactive Test Client for TCP Load Balancer
 *
 * Connects to the load balancer and provides an interactive
 * prompt to send commands to backend worker servers.
 *
 * Usage: client [host] [port]
 *   Defaults: host=127.0.0.1  port=8080
 */

#include "common.h"

/* ── Receive thread: print server responses ─────────────────────── */
typedef struct {
    sock_t fd;
    int    *running;
} recv_ctx_t;

#ifdef _WIN32
static DWORD WINAPI recv_thread(LPVOID arg) {
#else
static void *recv_thread(void *arg) {
#endif
    recv_ctx_t *ctx = (recv_ctx_t *)arg;
    char buf[BUF_SIZE];
    int  n;

    while (*(ctx->running)) {
        n = recv(ctx->fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (*(ctx->running)) {
                printf("\n[Disconnected from server]\n");
                *(ctx->running) = 0;
            }
            break;
        }
        buf[n] = '\0';
        printf("%s", buf);
        fflush(stdout);
    }

    return 0;
}

/* ── Main ───────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    const char *host = "127.0.0.1";
    int port = LB_PORT;
    sock_t fd;
    struct sockaddr_in addr;
    int running = 1;
    recv_ctx_t rctx;
    thread_t t;
    char input[BUF_SIZE];

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    if (sock_init() != 0) {
        fprintf(stderr, "Error: Socket initialization failed.\n");
        return 1;
    }

    /* Create socket and connect */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK) {
        fprintf(stderr, "Error: socket() failed (%d).\n", sock_errno());
        sock_cleanup();
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);

    if (INET_PTON(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Error: Invalid address '%s'.\n", host);
        CLOSE_SOCKET(fd);
        sock_cleanup();
        return 1;
    }

    printf("Connecting to load balancer at %s:%d ...\n", host, port);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCK_ERR) {
        fprintf(stderr, "Error: connect() failed (%d).\n", sock_errno());
        CLOSE_SOCKET(fd);
        sock_cleanup();
        return 1;
    }

    printf("Connected! Type commands (ECHO <msg>, TIME, UPPER <msg>, QUIT):\n\n");

    /* Start receive thread */
    rctx.fd      = fd;
    rctx.running = &running;

    if (thread_create(&t, recv_thread, &rctx) != 0) {
        fprintf(stderr, "Error: Thread creation failed.\n");
        CLOSE_SOCKET(fd);
        sock_cleanup();
        return 1;
    }
    thread_detach(t);

    /* Interactive input loop */
    while (running) {
        if (!fgets(input, sizeof(input), stdin))
            break;

        if (send(fd, input, (int)strlen(input), 0) <= 0) {
            printf("[Send failed, disconnected]\n");
            break;
        }

        /* Check for QUIT */
        {
            char tmp[BUF_SIZE];
            size_t tl;
            strncpy(tmp, input, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            tl = strlen(tmp);
            while (tl > 0 && (tmp[tl-1] == '\n' || tmp[tl-1] == '\r'))
                tmp[--tl] = '\0';

            if (strcmp(tmp, "QUIT") == 0 || strcmp(tmp, "quit") == 0) {
#ifdef _WIN32
                Sleep(500);
#else
                usleep(500000);
#endif
                break;
            }
        }

#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }

    running = 0;
    CLOSE_SOCKET(fd);
    sock_cleanup();
    printf("\nSession ended.\n");
    return 0;
}
