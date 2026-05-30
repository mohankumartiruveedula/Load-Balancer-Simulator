<div align="center">

# ⚡ TCP Load Balancer

**A high-performance, fault-tolerant TCP load balancer written in pure C with transparent failover and dynamic worker management.**

[![Language](https://img.shields.io/badge/Language-C-blue.svg)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)](#-cross-platform-support)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

<br/>

```
  Client 1 ──┐                             ┌── Worker :9001
  Client 2 ──┼──► [ LOAD BALANCER :8080 ] ──┼── Worker :9002
  Client 3 ──┘    Least-Connections Algo    └── Worker :9003
```

</div>

---

## 📋 Table of Contents

- [Overview](#-overview)
- [Features](#-features)
- [Architecture](#-architecture)
- [How It Works](#-how-it-works)
- [Getting Started](#-getting-started)
- [Usage](#-usage)
- [Project Structure](#-project-structure)
- [Technical Details](#-technical-details)
- [Cross-Platform Support](#-cross-platform-support)
- [Contributing](#-contributing)

---

## 🔍 Overview

This project implements a production-style **TCP Load Balancer** from scratch in C, demonstrating core concepts of distributed systems, network programming, and fault-tolerant design. The load balancer accepts incoming client connections on a single port and intelligently distributes them across a pool of backend worker servers using the **Least-Connections** scheduling algorithm.

Unlike simple round-robin implementations, this system features **transparent mid-session failover** — if a worker crashes while serving a client, the load balancer seamlessly migrates that session to a healthy worker without dropping the client connection.

---

## ✨ Features

| Feature | Description |
|---------|-------------|
| 🔄 **Least-Connections Routing** | Dynamically routes each client to the worker with the fewest active connections, ensuring even load distribution |
| 🛡️ **Transparent Failover** | Detects worker crashes mid-session and seamlessly re-routes clients to healthy workers without disconnection |
| 💓 **Proactive Health Checks** | Periodically probes downed workers and automatically restores them when they come back online |
| 🚀 **Auto Worker Spawning** | Load balancer automatically launches configured number of worker processes on startup |
| 🧵 **Multi-Threaded Proxy** | Uses `select()`-based I/O multiplexing per session for efficient bidirectional data relay |
| 📊 **Real-Time Web Dashboard** | Glassmorphic web UI (`http://localhost:8081`) streaming worker health, connection bars, and log timelines in real-time via Server-Sent Events (SSE) |
| 🖥️ **Cross-Platform** | Compiles and runs on both Windows (Winsock2) and Linux (POSIX sockets) with zero code changes |

---

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          SYSTEM ARCHITECTURE                           │
└─────────────────────────────────────────────────────────────────────────┘

  ┌──────────┐         ┌─────────────────────────────┐       ┌───────────────┐
  │ Client 1 │────┐    │      LOAD BALANCER          │   ┌──►│ Worker :9001  │
  └──────────┘    │    │      Port 8080               │   │   │ ECHO|TIME|    │
                  │    │                               │   │   │ UPPER|QUIT    │
  ┌──────────┐    ├───►│  ┌───────────────────────┐   ├───┤   └───────────────┘
  │ Client 2 │────┤    │  │    Worker Table        │   │   │
  └──────────┘    │    │  │  ┌───┬────────┬──────┐ │   │   │   ┌───────────────┐
                  │    │  │  │ # │ Active │Status│ │   │   ├──►│ Worker :9002  │
  ┌──────────┐    │    │  │  ├───┼────────┼──────┤ │   │   │   │ ECHO|TIME|    │
  │ Client 3 │────┘    │  │  │ 0 │   2    │ALIVE │ │   │   │   │ UPPER|QUIT    │
  └──────────┘         │  │  │ 1 │   0    │ALIVE │ │   │   │   └───────────────┘
                       │  │  │ 2 │   1    │ DOWN │ │   │   │
                       │  │  └───┴────────┴──────┘ │   │   │   ┌───────────────┐
                       │  └───────────────────────┘   │   └──►│ Worker :9003  │
                       │                               │       │ ECHO|TIME|    │
                       │  Algorithm: LEAST CONNECTIONS │       │ UPPER|QUIT    │
                       └─────────────────────────────┘       └───────────────┘
```

### Data Flow

```
1. Client ──► TCP Connect ──► Load Balancer (port 8080)
2. Load Balancer ──► select_worker() ──► Pick worker with min(active_conns)
3. Load Balancer ──► TCP Connect ──► Selected Worker (port 900x)
4. Bidirectional relay:  Client ◄──► Load Balancer ◄──► Worker
5. On disconnect: decrement worker.active_conns
```

### Failover Flow

```
Worker crashes mid-session
      │
      ▼
recv(worker_fd) returns error
      │
      ▼
mark_worker_dead(idx) ──► Set is_alive=0, record timestamp
      │
      ▼
select_worker() ──► Pick next healthy worker
      │
      ▼
connect_to_worker(new_idx) ──► Establish new backend connection
      │
      ▼
Notify client: "[LB] Server failed. Reconnecting..."
      │
      ▼
Resume relay loop with new worker_fd  ✓ Client never disconnects
```

---

## 📊 Real-Time Web Dashboard

In addition to the terminal-based output, the load balancer runs an embedded HTTP server on **port 8081** which serves a modern, responsive web dashboard.

- **Real-Time Data Streaming**: Uses **Server-Sent Events (SSE)** (`EventSource` API) to push events instantly from the C backend to the browser.
- **Glassmorphism UI**: Features a dark theme with glass-like panels, worker status cards, dynamic connection progress bars, and stats widgets.
- **Live Logs Timeline**: A scrolling terminal console in the web UI that displays real-time connection routing, failovers, and worker recoveries.
- **API Access**: Exposes `GET /api/status` returning a JSON snapshot of the cluster state.

To access the dashboard:
1. Open your browser and navigate to `http://localhost:8081`.
2. Connect clients and watch the routing and connection bars update in real-time.
3. Terminate a worker process to watch its card turn red (DOWN) and log the failover instantly.

---

## ⚙️ How It Works

### Least-Connections Algorithm

The load balancer maintains a global worker table protected by a mutex. On each new connection:

```c
// Simplified selection logic
for (i = 0; i < num_workers; i++) {
    if (!workers[i].is_alive) continue;           // skip dead workers
    if (workers[i].active_conns < workers[best].active_conns)
        best = i;                                  // pick least loaded
}
workers[best].active_conns++;                      // reserve slot
```

This ensures traffic naturally gravitates toward underloaded servers, unlike round-robin which is blind to actual server load.

### Transparent Proxy

The load balancer operates as a **Layer 4 (TCP) proxy**. It does **not** interpret application-level commands (`ECHO`, `TIME`, etc.). It simply relays raw bytes between client and worker using a `select()`-based event loop per session — making it protocol-agnostic and extensible.

### Health Recovery

Workers marked `DOWN` are periodically probed with a quick TCP connect. If the probe succeeds, the worker is automatically restored to the `ALIVE` pool — no manual intervention required.

---

## 🚀 Getting Started

### Prerequisites

- **GCC** (MinGW on Windows, or any standard GCC on Linux)
- **Make** (optional)

### Build

**Windows (MinGW):**
```bash
gcc -Wall -O2 -o load_balancer.exe load_balancer.c -lws2_32
gcc -Wall -O2 -o worker_server.exe worker_server.c -lws2_32
gcc -Wall -O2 -o client.exe client.c -lws2_32
```

**Linux:**
```bash
gcc -Wall -O2 -o load_balancer load_balancer.c -lpthread
gcc -Wall -O2 -o worker_server worker_server.c -lpthread
gcc -Wall -O2 -o client client.c -lpthread
```

### Run

```bash
# Terminal 1 — Start the load balancer (auto-spawns 3 workers)
./load_balancer

# Terminal 2 — Connect a client
./client

# Terminal 3 — Connect another client (will be routed to least-loaded worker)
./client
```

> **Note:** The load balancer automatically spawns worker server processes on startup. No need to start them manually.

---

## 💻 Usage

Once connected via the client, you have access to the following commands:

| Command | Description | Example |
|---------|-------------|---------|
| `ECHO <msg>` | Echoes your message back | `ECHO Hello World` → `ECHO: Hello World` |
| `TIME` | Returns current server timestamp | `TIME` → `TIME: 2026-05-30 19:12:00` |
| `UPPER <msg>` | Converts message to uppercase | `UPPER hello` → `UPPER: HELLO` |
| `QUIT` | Gracefully closes the session | `QUIT` → `Goodbye!` |

### Example Session

```
$ ./client
Connecting to load balancer at 127.0.0.1:8080 ...
Connected! Type commands (ECHO <msg>, TIME, UPPER <msg>, QUIT):

=== Worker Server ===
Commands: ECHO <msg> | TIME | UPPER <msg> | QUIT
> ECHO Hello from client!
ECHO: Hello from client!
> TIME
TIME: 2026-05-30 19:12:43
> UPPER distributed systems are awesome
UPPER: DISTRIBUTED SYSTEMS ARE AWESOME
> QUIT
Goodbye!

Session ended.
```

### Live Dashboard (Load Balancer Terminal)

```
+----------- Worker Dashboard -----------+
|  #  |    Address        | Active | Status |
+-----+-------------------+--------+--------+
|  0  | 127.0.0.1:9001    |    2   | ALIVE  |
|  1  | 127.0.0.1:9002    |    0   | ALIVE  |
|  2  | 127.0.0.1:9003    |    1   | DOWN   |
+-----+-------------------+--------+--------+
```

---

## 📁 Project Structure

```
tcp-load-balancer/
├── common.h              # Cross-platform socket & thread abstraction layer
├── load_balancer.c       # Central load balancer with failover logic
├── worker_server.c       # Backend worker server (ECHO/TIME/UPPER service)
├── client.c              # Interactive test client
├── DOCUMENTATION.md      # Detailed technical documentation
├── DEMO_GUIDE.md         # Step-by-step demo walkthrough
└── README.md             # This file
```

| File | Lines | Purpose |
|------|-------|---------|
| `common.h` | ~145 | Platform abstraction: sockets, threads, mutexes, logging |
| `load_balancer.c` | ~504 | Core router: worker selection, proxy relay, failover, health checks |
| `worker_server.c` | ~221 | Backend service: command parsing, client handling |
| `client.c` | ~149 | Test client: async recv thread + interactive input |

---

## 🔧 Technical Details

### Key Networking Concepts Used

- **TCP Socket Programming** — `socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `send()`, `recv()`
- **I/O Multiplexing** — `select()` for monitoring multiple file descriptors per session
- **Connection-Oriented Protocol** — Reliable, ordered data delivery via TCP's 3-way handshake
- **Transparent Proxying** — Layer 4 byte-stream relay without protocol awareness

### Concurrency Model

- **Thread-per-session** — Each client session gets a dedicated relay thread
- **Mutex-protected shared state** — Global worker table guarded by `CRITICAL_SECTION` (Win) / `pthread_mutex_t` (Linux)
- **Bidirectional select loop** — Single thread per session monitors both client and worker sockets simultaneously

### Fault Tolerance

- **3-pass worker selection**: (1) probe dead workers, (2) pick alive with least connections, (3) retry timed-out dead workers
- **Inline failover**: Worker failure detected inside the relay loop triggers immediate re-routing
- **Automatic recovery**: Downed workers are probed and restored without manual restart of the load balancer

---

## 🖥️ Cross-Platform Support

The project uses a custom abstraction layer (`common.h`) for full portability:

| Abstraction | Windows | Linux |
|-------------|---------|-------|
| Socket type | `SOCKET` (Winsock2) | `int` (POSIX) |
| Socket init | `WSAStartup()` | Not needed |
| Close socket | `closesocket()` | `close()` |
| Thread creation | `CreateThread()` | `pthread_create()` |
| Mutex | `CRITICAL_SECTION` | `pthread_mutex_t` |
| IP conversion | `inet_addr()`/`inet_ntoa()` | `inet_pton()`/`inet_ntop()` |
| Process spawn | `CreateProcess()` | `fork()`/`execl()` |

---

## 🤝 Contributing

Contributions are welcome! Feel free to open issues or submit pull requests.

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

---

<div align="center">

**Built with ❤️ in C — No frameworks, no libraries, just sockets and threads.**

</div>
