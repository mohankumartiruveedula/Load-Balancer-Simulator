/*
 * worker_server.c - Backend Worker Server for TCP Load Balancer
 *
 * Provides a simple text-based service:
 *   ECHO <msg>   -- echoes the message back
 *   TIME          -- returns the current server timestamp
 *   UPPER <msg>   -- returns the message in uppercase
 *   QUIT          -- closes the connection
 *
 * Usage: worker_server <port>
 *   e.g. worker_server 9001
 */

#include "common.h"
#include <ctype.h>

/* ── Per-client context passed to handler thread ────────────────── */
typedef struct {
    sock_t client_fd;
    struct sockaddr_in addr;
    int    worker_port;
} client_ctx_t;

/* ── Trim trailing \r\n ─────────────────────────────────────────── */
static void trim_crlf(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

/* ── Command handlers ───────────────────────────────────────────── */
static void cmd_echo(sock_t fd, const char *arg) {
    char resp[BUF_SIZE];
    snprintf(resp, sizeof(resp), "ECHO: %s\n", arg);
    send(fd, resp, (int)strlen(resp), 0);
}

static void cmd_time(sock_t fd) {
    char resp[BUF_SIZE];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(resp, sizeof(resp),
             "TIME: %04d-%02d-%02d %02d:%02d:%02d\n",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
    send(fd, resp, (int)strlen(resp), 0);
}

static void cmd_upper(sock_t fd, const char *arg) {
    char resp[BUF_SIZE];
    size_t i, off;
    snprintf(resp, sizeof(resp), "UPPER: ");
    off = strlen(resp);
    for (i = 0; arg[i] && off + 1 < sizeof(resp) - 1; i++, off++)
        resp[off] = (char)toupper((unsigned char)arg[i]);
    resp[off++] = '\n';
    resp[off]   = '\0';
    send(fd, resp, (int)strlen(resp), 0);
}

static void cmd_unknown(sock_t fd) {
    const char *msg =
        "ERROR: Unknown command.\n"
        "Available commands: ECHO <msg> | TIME | UPPER <msg> | QUIT\n";
    send(fd, msg, (int)strlen(msg), 0);
}

/* ── Client handler thread ──────────────────────────────────────── */
#ifdef _WIN32
static DWORD WINAPI handle_client(LPVOID arg) {
#else
static void *handle_client(void *arg) {
#endif
    client_ctx_t *ctx = (client_ctx_t *)arg;
    sock_t fd = ctx->client_fd;
    char client_ip[64];
    INET_NTOP(AF_INET, &ctx->addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(ctx->addr.sin_port);
    int wport = ctx->worker_port;
    free(ctx);

    LOG("WORK", "Worker :%d  |  Client %s:%d connected", wport, client_ip, client_port);

    /* Send welcome banner */
    const char *banner =
        "=== Worker Server ===\n"
        "Commands: ECHO <msg> | TIME | UPPER <msg> | QUIT\n"
        "> ";
    send(fd, banner, (int)strlen(banner), 0);

    char buf[BUF_SIZE];
    int  n;
    while ((n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        trim_crlf(buf);

        if (strlen(buf) == 0) {
            send(fd, "> ", 2, 0);
            continue;
        }

        LOG("WORK", "Worker :%d  |  %s:%d  ->  \"%s\"", wport, client_ip, client_port, buf);

        /* Parse command (case-insensitive prefix match) */
        if (strncmp(buf, "ECHO ", 5) == 0 || strncmp(buf, "echo ", 5) == 0) {
            cmd_echo(fd, buf + 5);
        } else if (strcmp(buf, "TIME") == 0 || strcmp(buf, "time") == 0) {
            cmd_time(fd);
        } else if (strncmp(buf, "UPPER ", 6) == 0 || strncmp(buf, "upper ", 6) == 0) {
            cmd_upper(fd, buf + 6);
        } else if (strcmp(buf, "QUIT") == 0 || strcmp(buf, "quit") == 0) {
            const char *bye = "Goodbye!\n";
            send(fd, bye, (int)strlen(bye), 0);
            break;
        } else {
            cmd_unknown(fd);
        }
        send(fd, "> ", 2, 0);
    }

    LOG("WORK", "Worker :%d  |  Client %s:%d disconnected", wport, client_ip, client_port);
    CLOSE_SOCKET(fd);
    return 0;
}

/* ── Main ───────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    int port;
    sock_t server_fd;
    int opt = 1;
    struct sockaddr_in addr;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid port number.\n");
        return 1;
    }

    if (sock_init() != 0) {
        fprintf(stderr, "Error: Socket initialization failed.\n");
        return 1;
    }

    /* Create listening socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCK) {
        fprintf(stderr, "Error: socket() failed (%d).\n", sock_errno());
        sock_cleanup();
        return 1;
    }

    /* Allow address reuse */
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((unsigned short)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCK_ERR) {
        fprintf(stderr, "Error: bind() failed on port %d (%d).\n", port, sock_errno());
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

    printf("============================================\n");
    printf("   Worker Server listening on port %d\n", port);
    printf("   Service: ECHO | TIME | UPPER | QUIT\n");
    printf("============================================\n\n");

    /* Accept loop */
    for (;;) {
        struct sockaddr_in client_addr;
        int addrlen = sizeof(client_addr);
        sock_t client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (client_fd == INVALID_SOCK) {
            LOG("WORK", "accept() failed (%d), continuing...", sock_errno());
            continue;
        }

        {
            client_ctx_t *ctx = (client_ctx_t *)malloc(sizeof(client_ctx_t));
            if (!ctx) {
                CLOSE_SOCKET(client_fd);
                continue;
            }
            ctx->client_fd   = client_fd;
            ctx->addr        = client_addr;
            ctx->worker_port = port;

            {
                thread_t t;
                if (thread_create(&t, handle_client, ctx) != 0) {
                    LOG("WORK", "Thread creation failed, dropping client.");
                    free(ctx);
                    CLOSE_SOCKET(client_fd);
                    continue;
                }
                thread_detach(t);
            }
        }
    }

    CLOSE_SOCKET(server_fd);
    sock_cleanup();
    return 0;
}
