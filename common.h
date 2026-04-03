/*
 * common.h - Shared definitions for TCP Load Balancer Project
 * Cross-platform socket abstractions and shared constants.
 * Compatible with MinGW GCC 6+.
 */

#ifndef COMMON_H
#define COMMON_H

/* ── Cross-platform socket abstraction ──────────────────────────── */
#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601  /* Windows 7+ */
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>

    typedef SOCKET sock_t;
    #define INVALID_SOCK INVALID_SOCKET
    #define CLOSE_SOCKET closesocket
    #define SOCK_ERR     SOCKET_ERROR

    static inline int sock_init(void) {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    static inline void sock_cleanup(void) { WSACleanup(); }
    static inline int  sock_errno(void)   { return WSAGetLastError(); }

    /*
     * MinGW GCC 6 does not expose inet_pton / inet_ntop.
     * We provide simple IPv4-only shims that work everywhere.
     */
    static inline int compat_inet_pton(int af, const char *src, void *dst) {
        if (af != AF_INET) return -1;
        unsigned long addr = inet_addr(src);
        if (addr == INADDR_NONE && strcmp(src, "255.255.255.255") != 0)
            return 0;
        memcpy(dst, &addr, sizeof(addr));
        return 1;
    }

    static inline const char *compat_inet_ntop(int af, const void *src,
                                               char *dst, int size) {
        if (af != AF_INET) return NULL;
        struct in_addr in;
        memcpy(&in, src, sizeof(in));
        char *s = inet_ntoa(in);
        if ((int)strlen(s) >= size) return NULL;
        strcpy(dst, s);
        return dst;
    }

    #define INET_PTON  compat_inet_pton
    #define INET_NTOP  compat_inet_ntop

#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <errno.h>

    typedef int sock_t;
    #define INVALID_SOCK (-1)
    #define CLOSE_SOCKET close
    #define SOCK_ERR     (-1)

    static inline int  sock_init(void)    { return 0; }
    static inline void sock_cleanup(void) { (void)0; }
    static inline int  sock_errno(void)   { return errno; }

    #define INET_PTON  inet_pton
    #define INET_NTOP  inet_ntop
#endif

/* ── Common includes ────────────────────────────────────────────── */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Thread abstraction ─────────────────────────────────────────── */
#ifdef _WIN32
    #include <windows.h>
    typedef HANDLE thread_t;
    typedef DWORD (WINAPI *thread_fn_t)(LPVOID);

    static inline int thread_create(thread_t *t, DWORD (WINAPI *fn)(LPVOID), void *arg) {
        *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
        return (*t == NULL) ? -1 : 0;
    }
    static inline void thread_detach(thread_t t) { CloseHandle(t); }

    /* Simple mutex */
    typedef CRITICAL_SECTION mutex_t;
    #define MUTEX_INIT(m)    InitializeCriticalSection(&(m))
    #define MUTEX_LOCK(m)    EnterCriticalSection(&(m))
    #define MUTEX_UNLOCK(m)  LeaveCriticalSection(&(m))
    #define MUTEX_DESTROY(m) DeleteCriticalSection(&(m))
#else
    #include <pthread.h>
    typedef pthread_t thread_t;

    static inline int thread_create(thread_t *t, void *(*fn)(void*), void *arg) {
        return pthread_create(t, NULL, fn, arg);
    }
    static inline void thread_detach(thread_t t) { pthread_detach(t); }

    typedef pthread_mutex_t mutex_t;
    #define MUTEX_INIT(m)    pthread_mutex_init(&(m), NULL)
    #define MUTEX_LOCK(m)    pthread_mutex_lock(&(m))
    #define MUTEX_UNLOCK(m)  pthread_mutex_unlock(&(m))
    #define MUTEX_DESTROY(m) pthread_mutex_destroy(&(m))
#endif

/* ── Constants ──────────────────────────────────────────────────── */
#define LB_PORT          8080        /* Load balancer listen port     */
#define MAX_WORKERS      10          /* Maximum backend workers       */
#define BUF_SIZE         4096        /* I/O buffer size               */
#define MAX_PENDING      64          /* listen() backlog              */

/* ── Worker descriptor ──────────────────────────────────────────── */
typedef struct {
    char   ip[46];                   /* worker IP (IPv4/IPv6 string)  */
    int    port;                     /* worker port                   */
    int    active_conns;             /* current active connections    */
    int    total_served;             /* lifetime connections served   */
    int    is_alive;                 /* 1 = healthy, 0 = down         */
    time_t dead_since;               /* time() when marked dead       */
} worker_info_t;

/* ── Utility ────────────────────────────────────────────────────── */
#define LOG(tag, fmt, ...) \
    do { \
        time_t _t = time(NULL); \
        struct tm *_tm = localtime(&_t); \
        char _ts[20]; \
        strftime(_ts, sizeof(_ts), "%H:%M:%S", _tm); \
        printf("[%s][%-4s] " fmt "\n", _ts, tag, ##__VA_ARGS__); \
        fflush(stdout); \
    } while(0)

#endif /* COMMON_H */
