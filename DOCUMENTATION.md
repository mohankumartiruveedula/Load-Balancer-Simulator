# TCP Load Balancer — Project Documentation

## 1. Introduction

This project implements a **TCP Load Balancer** in C that demonstrates core concepts of distributed system design. A central load balancer server accepts incoming client connections and intelligently distributes them across multiple backend worker servers using the **Least-Connections** algorithm, ensuring efficient and balanced resource utilization.

The worker servers provide a simple text-based service (echo, time, uppercase conversion) to demonstrate real request handling.

---

## 2. Key Concepts

### 2.1 Load Balancing

Load balancing is the process of distributing network traffic across multiple servers to ensure no single server is overwhelmed. It is a fundamental component in distributed systems and is used by all large-scale web services (Google, Amazon, Netflix).

**Why it matters:**
- **Reliability:** If one server fails, others continue serving clients.  
- **Scalability:** More servers can be added to handle increased traffic.  
- **Performance:** Clients are served faster because work is spread evenly.

### 2.2 Least-Connections Algorithm

Among load balancing strategies, the **Least-Connections** algorithm is one of the most effective for general use. It works as follows:

1. The load balancer maintains a **count of active connections** for each worker.
2. When a new client connects, the worker with the **fewest active connections** is selected.
3. The connection count is **incremented** when a client is assigned and **decremented** when the client disconnects.

**Advantages over Round-Robin:**

| Feature               | Round-Robin              | Least-Connections            |
|------------------------|--------------------------|------------------------------|
| Selection criteria     | Sequential rotation      | Fewest active connections    |
| Adapts to server load  | No                       | Yes                          |
| Handles slow clients   | Poorly (uneven buildup)  | Well (load-aware)            |
| Implementation         | Simpler                  | Slightly more complex        |

### 2.3 TCP (Transmission Control Protocol)

TCP is a **connection-oriented, reliable** transport protocol. Key properties used in this project:

- **3-Way Handshake:** Establishes a connection before data transfer (SYN → SYN-ACK → ACK).
- **Reliable Delivery:** Guarantees data arrives in order and without loss (using acknowledgments and retransmissions).
- **Stream-Oriented:** Data is sent as a continuous stream of bytes.

All communication in this project (client ↔ load balancer ↔ worker) uses TCP sockets.

### 2.4 Multi-Threading

Each client connection is handled in a **separate thread**, allowing the servers to serve multiple clients simultaneously without blocking. The project uses:
- **Windows:** `CreateThread()` API
- **Linux:** `pthread_create()` (POSIX threads)

### 2.5 Proxy Architecture

The load balancer acts as a **transparent TCP proxy**. It does not interpret the application-level protocol — it simply relays raw bytes between the client and the selected worker using two relay threads per session:
- **Thread 1:** Client → Worker (forwards client requests)
- **Thread 2:** Worker → Client (forwards worker responses)

---

## 3. System Architecture

```
                        ┌─────────────────────────┐
                        │     LOAD BALANCER        │
    ┌──────────┐        │     Port 8080            │        ┌──────────────┐
    │ Client 1 │───────►│                          │───────►│ Worker 9001  │
    └──────────┘        │  ┌──────────────────┐    │        │ ECHO|TIME|   │
                        │  │  Worker Table     │    │        │ UPPER|QUIT   │
    ┌──────────┐        │  │  ────────────     │    │        └──────────────┘
    │ Client 2 │───────►│  │  [0] 127.0.0.1:  │    │
    └──────────┘        │  │      9001 (2 conn)│    │        ┌──────────────┐
                        │  │  [1] 127.0.0.1:  │    │───────►│ Worker 9002  │
    ┌──────────┐        │  │      9002 (0 conn)│    │        │ ECHO|TIME|   │
    │ Client 3 │───────►│  │  [2] 127.0.0.1:  │    │        │ UPPER|QUIT   │
    └──────────┘        │  │      9003 (1 conn)│    │        └──────────────┘
                        │  └──────────────────┘    │
                        │                          │        ┌──────────────┐
                        │  Algorithm:              │───────►│ Worker 9003  │
                        │  LEAST CONNECTIONS        │        │ ECHO|TIME|   │
                        └─────────────────────────┘        │ UPPER|QUIT   │
                                                           └──────────────┘
```

### Data Flow

1. **Client** connects to Load Balancer on port 8080.
2. **Load Balancer** checks the worker table for the server with fewest active connections.
3. Load Balancer connects to the selected **Worker** and spawns two relay threads.
4. All data flows transparently: Client ↔ Load Balancer ↔ Worker.
5. When the client disconnects, the worker's active connection count is decremented.

---

## 4. File Structure

```
CN PROJECT/
├── common.h            # Cross-platform socket & thread abstractions
├── load_balancer.c     # Central load balancer (least-connections)
├── worker_server.c     # Backend worker server (ECHO/TIME/UPPER)
├── client.c            # Interactive test client
├── Makefile            # Build system
├── load_balancer.exe   # Compiled load balancer
├── worker_server.exe   # Compiled worker server
└── client.exe          # Compiled client
```

---

## 5. Code Walkthrough

### 5.1 common.h — Platform Abstraction Layer

This header provides a unified API that works on both Windows and Linux:

| Abstraction     | Windows Implementation   | Linux Implementation |
|-----------------|--------------------------|----------------------|
| Socket type     | `SOCKET` (Winsock)       | `int` (POSIX)        |
| Socket init     | `WSAStartup()`           | Not needed           |
| Close socket    | `closesocket()`          | `close()`            |
| Thread creation | `CreateThread()`         | `pthread_create()`   |
| Mutex           | `CRITICAL_SECTION`       | `pthread_mutex_t`    |
| IP conversion   | `inet_addr()`/`inet_ntoa()` | `inet_pton()`/`inet_ntop()` |

It also defines shared constants (buffer size, max workers, port numbers) and a `LOG()` macro for timestamped console output.

### 5.2 worker_server.c — Backend Service

**Startup:** Takes a port number as argument, creates a TCP listening socket, and enters an accept loop.

**Per-client handling:** Each accepted connection spawns a new thread (`handle_client`) that:
1. Sends a welcome banner with available commands
2. Reads client input in a loop
3. Parses and dispatches commands:
   - `ECHO <msg>` — sends the message back
   - `TIME` — sends the current date and time
   - `UPPER <msg>` — converts the message to uppercase
   - `QUIT` — sends goodbye and closes
4. Logs all activity with timestamps

### 5.3 load_balancer.c — Central Router

**Core data structure:** A global `workers[]` array stores each worker's IP, port, active connections, and total served count, protected by a mutex.

**Least-Connections selection (`select_worker`):**
```c
best = 0;
for (i = 1; i < num_workers; i++) {
    if (workers[i].active_conns < workers[best].active_conns)
        best = i;
}
workers[best].active_conns++;
```
This O(n) scan runs under a mutex lock to ensure thread safety.

**Proxy relay:** For each client, the load balancer:
1. Connects to the chosen worker
2. Creates two `relay_ctx_t` structures
3. Spawns two threads that call `recv()` → `send()` in a loop
4. On disconnect, decrements the worker's active count

**Dashboard:** After each routing decision and session end, a formatted table is printed showing all workers' current and total connections.

### 5.4 client.c — Test Client

Connects to the load balancer on port 8080 and spawns a background thread to print incoming server responses. The main thread reads user input from `stdin` and sends it to the server. This two-thread design allows asynchronous display of responses while the user types.

---

## 6. How to Build and Run

### Build
```bash
gcc -Wall -O2 -o worker_server.exe worker_server.c -lws2_32
gcc -Wall -O2 -o load_balancer.exe load_balancer.c -lws2_32
gcc -Wall -O2 -o client.exe client.c -lws2_32
```

### Run (5 terminals)
```
Terminal 1:  worker_server.exe 9001
Terminal 2:  worker_server.exe 9002
Terminal 3:  worker_server.exe 9003
Terminal 4:  load_balancer.exe
Terminal 5:  client.exe
```

### Example Session
```
> ECHO Hello World
ECHO: Hello World
> TIME
TIME: 2026-04-01 23:12:00
> UPPER distributed systems
UPPER: DISTRIBUTED SYSTEMS
> QUIT
Goodbye!
```

---

## 7. Conclusion

This project demonstrates the fundamentals of:
- **Load balancing** with the Least-Connections algorithm
- **TCP socket programming** with multi-threaded connection handling
- **Proxy design** with bidirectional data relay
- **Distributed system architecture** with a central coordinator and worker pool
- **Cross-platform C development** using abstraction layers

The system is extensible — additional workers can be added by modifying the worker table in `load_balancer.c`, and new services can be added to the workers by implementing additional command handlers.
