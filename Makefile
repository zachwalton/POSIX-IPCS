CC=gcc
CFLAGS=-Wall -O3
LDFLAGS=-lrt

all: pipcs

pipcs: pipcs.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm pipcs
