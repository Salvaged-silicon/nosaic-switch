# nosaic, for architectures the Go toolchain cannot target.
#
# Built the same way the datapath is, because it is C for a switch and the
# package builder already knows how to cross-compile that. CROSS_COMPILE comes
# from the recipe.
.NOTPARALLEL:

CC      = $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wno-unused-parameter
# The board controller sits at 0xea000000, which does not fit in a signed
# 32-bit off_t -- and this board is 32-bit. Without large file offsets every
# pread of it fails and the CLI reports that no controller answered, which is
# true of the call and not of the hardware.
CFLAGS += -D_FILE_OFFSET_BITS=64
LDFLAGS ?=

OBJS = nosaic.o hal_as5610.o config.o asic.o

all: nosaic

nosaic: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

nosaic.o:      nosaic.c hal.h
hal_as5610.o:  hal_as5610.c hal.h

strip: nosaic
	$(CROSS_COMPILE)strip -s nosaic

clean:
	rm -f *.o nosaic

.PHONY: all clean strip
