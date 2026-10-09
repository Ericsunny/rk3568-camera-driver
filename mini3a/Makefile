# mini3a — 板上直接编译(gcc,无需交叉)
CC ?= gcc
CFLAGS ?= -Wall -O2

mini3a: mini3a.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread

clean:
	rm -f mini3a

.PHONY: all clean
all: mini3a
