# mini3a — 板上直接编译(gcc,无需交叉)
CC ?= gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -O2
ifneq ($(BSP_UAPI),)
CPPFLAGS += -idirafter $(BSP_UAPI)
endif

mini3a: mini3a.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) -pthread

clean:
	rm -f mini3a

.PHONY: all clean
all: mini3a
