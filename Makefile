# TCP Load Balancer - Makefile
# Supports Windows (MinGW gcc) and Linux

CC      = gcc
CFLAGS  = -Wall -Wextra -O2

# Platform-specific linker flags
ifeq ($(OS),Windows_NT)
    LDFLAGS = -lws2_32
    EXT     = .exe
else
    LDFLAGS = -lpthread
    EXT     =
endif

# Targets
all: load_balancer$(EXT) worker_server$(EXT) client$(EXT)

load_balancer$(EXT): load_balancer.c common.h
	$(CC) $(CFLAGS) -o $@ load_balancer.c $(LDFLAGS)

worker_server$(EXT): worker_server.c common.h
	$(CC) $(CFLAGS) -o $@ worker_server.c $(LDFLAGS)

client$(EXT): client.c common.h
	$(CC) $(CFLAGS) -o $@ client.c $(LDFLAGS)

clean:
ifeq ($(OS),Windows_NT)
	del /Q load_balancer.exe worker_server.exe client.exe 2>nul || echo clean done
else
	rm -f load_balancer worker_server client
endif

.PHONY: all clean
