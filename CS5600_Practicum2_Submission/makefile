# Makefile for building client and server programs

# Compiler and flags
CC = gcc
CFLAGS = -Wall

# Targets
all: server client

server: server.c
	$(CC) $(CFLAGS) server.c -o server -lpthread

client: client.c
	$(CC) $(CFLAGS) client.c -o client

# Clean up build artifacts
clean:
	rm -f server client *.o
