# nosd-tdp — Trident+ (BCM56840 family) datapath.
#
# Only the BDE and its probe so far: enough to establish that userspace
# register access to this chip is correct before the SDK is allowed near it.
# CROSS_COMPILE is honoured because the package builder passes it.
CC      = $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter
# Shared with the other ASIC datapaths: MMIO ordering that differs per host
# architecture and has no business being written twice.
CFLAGS  += -I../common
LDFLAGS ?=

all: tdp-probe

tdp-probe: probe.o bde.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

probe.o: probe.c bde.h ../common/mmio.h
bde.o:   bde.c   bde.h ../common/mmio.h

strip: tdp-probe
	$(CROSS_COMPILE)strip -s tdp-probe

clean:
	rm -f *.o tdp-probe

.PHONY: all clean strip
