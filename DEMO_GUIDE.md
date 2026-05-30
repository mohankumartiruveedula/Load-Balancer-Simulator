# TCP Load Balancer — Demo Guide

## Prerequisites
- All executables built: `worker_server.exe`, `load_balancer.exe`, `client.exe`
- 5 terminal windows open (Command Prompt or PowerShell)

---

## Demo Script

### 1. Start Workers (Terminals 1–3)

Open 3 separate terminals and run:

```
Terminal 1:  worker_server.exe 9001
Terminal 2:  worker_server.exe 9002
Terminal 3:  worker_server.exe 9003
```

Each will display:
```
============================================
   Worker Server listening on port 900X
   Service: ECHO | TIME | UPPER | QUIT
============================================
```

### 2. Start Load Balancer (Terminal 4)

```
load_balancer.exe
```

You'll see:
```
========================================================
     TCP LOAD BALANCER (Least Connections Algorithm)
     Listening on port 8080
========================================================
  Workers:
    [0] 127.0.0.1:9001
    [1] 127.0.0.1:9002
    [2] 127.0.0.1:9003
========================================================
```

### 3. Connect First Client (Terminal 5)

```
client.exe
```

**Demo commands to type:**
```
ECHO Hello, this is Client 1
TIME
UPPER distributed systems
```

**Point out:**
- Load Balancer terminal shows routing to Worker 9001 (all workers had 0 connections, so first was picked)
- Worker 9001 terminal shows the client's commands being processed
- Load Balancer dashboard shows Worker 9001 has 1 active connection

### 4. Connect Second Client (Terminal 6 — open one more)

```
client.exe
```

**Point out:**
- Load Balancer routes this client to Worker 9002 (Worker 9001 has 1 conn, 9002 and 9003 have 0)
- Dashboard now shows: Worker 9001 = 1, Worker 9002 = 1, Worker 9003 = 0

### 5. Connect Third Client (Terminal 7 — open one more)

```
client.exe
```

**Point out:**
- Routed to Worker 9003 (the only one with 0 connections)
- Dashboard: All three workers have 1 connection each — perfectly balanced!

### 6. Demonstrate Least-Connections Recovery

Disconnect Client 1 by typing `QUIT` in its terminal.

**Point out:**
- Worker 9001's count drops back to 0
- Connect a new client → it goes to Worker 9001 (now has fewest connections)

---

## Key Points to Emphasize During Demo

1. **Least-Connections is adaptive** — unlike Round-Robin, it responds to actual server load
2. **TCP proxy is transparent** — the client doesn't know which worker it's talking to
3. **Multi-threaded design** — each connection runs independently without blocking others
4. **Real-time dashboard** — shows active connection distribution across workers
5. **Graceful handling** — disconnection properly updates worker counts

---

## Potential Questions & Answers

**Q: Why Least-Connections over Round-Robin?**
A: Round-Robin blindly rotates, so if some clients have long sessions, workers become unevenly loaded. Least-Connections adapts to real load.

**Q: How does the proxy relay work?**
A: Two threads per session — one forwards client→worker, the other forwards worker→client. The load balancer doesn't parse the data, just relays raw bytes.

**Q: Is this thread-safe?**
A: Yes — the worker table is protected by a mutex. Only one thread can read/write connection counts at a time.

**Q: What happens if a worker goes down?**
A: The load balancer will fail to connect and report an error. The client gets an "ERROR: Worker unavailable" message.
