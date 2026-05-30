# TCP Load Balancer — Presentation Guide

This guide is designed to help your team of 4 understand the system from top to bottom and prepare for your presentation.

## 🌟 Project Overview (The "Elevator Pitch")
You have built a **TCP Load Balancer in C**. In simple terms, it is a "middleman" server that sits between clients (users) and backend worker servers. Instead of clients connecting directly to a worker, they connect to your Load Balancer, which smartly distributes the incoming traffic across multiple workers. 

This is the exact fundamental concept used by large-scale applications like Google, Netflix, and Amazon to ensure their websites don't crash when millions of users connect at once.

---

## 🏗️ Core Architecture (The 3 Main Components)

Your system is made up of three distinct pieces of software communicating over the network:

1. **The Client (`client.c`):** 
   A simple terminal application that users type into. It connects to the Load Balancer and sends requests. It has two threads so it can send messages and receive responses at the same time.
2. **The Worker Servers (`worker_server.c`):** 
   These are the actual "backend" servers doing the work. In a real company, these would be web servers or database servers. In your project, they provide 4 simple services to prove the system works:
   - `ECHO` (repeats your message)
   - `TIME` (gives the current server time)
   - `UPPER` (turns your message into UPPERCASE)
   - `QUIT` (closes the connection)
3. **The Central Load Balancer (`load_balancer.c`):** 
   The most important piece. It listens on port 8080. It manages a "table" of all available worker servers. When a client connects, the load balancer decides which worker should handle that client, and then acts as a transparent proxy (relaying bytes back and forth between the client and the chosen worker).

---

## 🧠 The "Smart" Features (Key Selling Points for the Presentation)

These are the technical highlights you should focus on during your presentation, as they show off the complexity of the project:

* **The Routing Algorithm (Least-Connections):** 
  Instead of just assigning clients to workers blindly (like Round-Robin), your load balancer keeps track of *exactly how many active clients* each worker is currently handling. When a new client arrives, it assigns them to the worker with the **fewest active connection count**. This prevents any single server from getting overwhelmed.
* **Transparent Failover (Handling Crashes):** 
  This is a huge feature. If a worker server suddenly crashes or dies while a client is actively talking to it, the Load Balancer detects the broken pipe. *Without dropping the client's connection*, the Load Balancer seamlessly reconnects the client to the next healthy working server. The client just sees a warning message, but doesn't have to restart their app.
* **Proactive Health Checks:**
  If a server goes down, the Load Balancer marks it as `DOWN`. In the background, it periodically sends "probes" to dead servers. If a server is restarted and comes back online, the Load Balancer automatically detects this and adds it back to the workforce.
* **Auto-Spawning:**
  When you run the load balancer, it natively uses OS APIs (`CreateProcess` on Windows, `fork/execl` on Linux) to automatically spin up 3 worker servers in the background. You don't have to manually start 4 different terminals just to boot the system.
* **Multi-Threading:** 
  The system handles multiple clients simultaneously using threads. Every time a client connects, the Load Balancer spawns two relay threads: one to read from the client and send to the worker, and one to read from the worker and send to the client.

---

## 🗣️ Suggested Presentation Breakdown (For 4 Team Members)

Since you have 4 people, here is a logical way you can divide the presentation topics so everyone has a strong technical part to talk about:

### Speaker 1: Introduction & Architecture Overview
* What is Load Balancing and why is it essential for modern networks?
* Introduce the 3 components of your project (Client, Load Balancer, Workers).
* Show the visual architecture diagram (from your `DOCUMENTATION.md`).

### Speaker 2: The Load Balancing Algorithm
* Explain the "Least-Connections" algorithm.
* Contrast it with simpler algorithms like "Round-Robin" (Sequential rotation). Explain *why* Least-Connections is better (it adapts dynamically to slow servers or long-lasting connections).
* Explain how the Load Balancer uses a Mutex (lock) to safely update the connection counts in the worker table without race conditions.

### Speaker 3: Fault Tolerance & Recovery (The Coolest Part)
* Explain what happens when a worker server crashes.
* Detail the **Transparent Failover**: How the Load Balancer catches the error in the middle of a `select()` loop, drops the bad socket, connects to a new worker, and resumes the relay without dropping the client.
* Explain the built-in Health Checks that bring servers back to life.

### Speaker 4: System Implementation & Multi-threading
* Discuss how TCP Sockets were used to build this (connections, listening, accepting).
* Explain the "Transparent Proxy" design: The fact that the Load Balancer doesn't understand the "ECHO" or "TIME" commands, it just relays raw bytes (using `recv()` and `send()`) via dedicated relay threads.
* Talk about the cross-platform nature of your code (how you handled both Windows `CreateThread` and Linux `pthread_create`).
