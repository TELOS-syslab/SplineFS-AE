CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

.PHONY: all clean

all: locality_sweep_bench

locality_sweep_bench: locality_sweep_bench.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f locality_sweep_bench
