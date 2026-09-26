# nosd-fm6000, and the probe tool that comes with it.
#
# The simplest datapath makefile in this tree, and for a good reason: there is
# no SDK. Every other ASIC here links against openbcm -- 874 MB of static
# archives that have to be staged before anything compiles -- and this one
# links against libc and libpthread. A contributor with no vendor tree at all
# can build this in full, which is not true of any other board.
#
# CROSS_COMPILE is honoured because the package builder passes it, and ignoring
# it is not a harmless default: it silently produces a binary linked against the
# build container's glibc rather than the one the image ships, which builds
# cleanly, packages cleanly, and fails on the switch with a symbol version error
# that names neither cause.
#
# The recipe asks for `clean build strip` in one invocation and the package
# builder passes -j. Those are a sequence, not independent goals: run in
# parallel, clean deletes a binary another target has just decided is up to
# date, and strip then fails on a file that is not there.
.NOTPARALLEL:

CC      = $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter
# Shared with the other ASIC datapaths: MMIO ordering that differs per host
# architecture and has no business being written twice. Only mmio.h, props.c
# and portmode.c in there are chip-agnostic -- tapbridge.c, query.c, l3sync.c
# and acl.c are all built on bcm_* calls and do not apply to this chip.
CFLAGS  += -I../common
CFLAGS  += -DNOSAIC_QUERY_DRIVER=\"fm6000\"
LDLIBS  += -lpthread

COMMON_OBJS = pci.o sbus.o
DAEMON_OBJS = main.o boot.o sock.o $(COMMON_OBJS)
PROBE_OBJS  = probe.o boot.o $(COMMON_OBJS)

all: nosd-fm6000 fm6000-probe

nosd-fm6000: $(DAEMON_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(DAEMON_OBJS) $(LDLIBS)

# A separate binary on purpose. At the point where somebody needs to look at
# this chip, the daemon is very likely the thing that is wrong -- so the tool
# for looking at it must not be part of the daemon.
fm6000-probe: $(PROBE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(PROBE_OBJS) $(LDLIBS)

main.o: main.c pci.h regs.h boot.h sock.h
pci.o:  pci.c pci.h regs.h ../common/mmio.h
boot.o: boot.c boot.h pci.h regs.h
sock.o: sock.c sock.h pci.h regs.h
sbus.o: sbus.c sbus.h pci.h regs.h
probe.o: probe.c pci.h regs.h boot.h

# Stripping is a separate target rather than -s at link time, so a debug build
# is one target away rather than a makefile edit. Unlike the Broadcom datapaths
# it is not a size emergency here -- there is no SDK to carry, and these are
# tens of kilobytes rather than hundreds of megabytes.
strip: nosd-fm6000 fm6000-probe
	$(CROSS_COMPILE)strip nosd-fm6000 fm6000-probe
	@echo "    nosd-fm6000 $$(stat -c%s nosd-fm6000) bytes, fm6000-probe $$(stat -c%s fm6000-probe) bytes"

clean:
	rm -f nosd-fm6000 fm6000-probe *.o ../common/*.o

.PHONY: all clean strip
