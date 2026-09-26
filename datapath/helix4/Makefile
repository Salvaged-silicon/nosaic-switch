# nosd-helix4 — Helix4 (BCM56340) datapath, for boards whose CMIC is on the
# CPU's own die rather than behind PCI.
#
# The recipe asks for `clean nosd-helix4 strip` in one invocation and the
# package builder passes -j. Those are a sequence, not independent goals: run
# in parallel, clean deletes the binary another target has just decided is up
# to date, and strip then fails on a file that is not there.
.NOTPARALLEL:

CC      = $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter
# Shared with the other ASIC datapaths: MMIO ordering, the DMA allocator, the
# property reader, the tap bridge and the FIB mirror.
CFLAGS  += -I../common
LDFLAGS ?=
LDLIBS  += -lpthread

# The SDK is optional at build time. `make` alone builds the BDE and the probe,
# which is what a contributor without the SDK staged can do and what CI runs;
# SDK_DIR turns on the vector layer that lets the SDK drive the chip.
ifneq ($(SDK_DIR),)
# THE SDK'S OWN DEFINES, CAPTURED BY ITS BUILD.
#
# Anything linking against the SDK must compile with the same set: the headers
# are shared, so a define set for the library and not for us silently changes
# struct layouts and enum values, with no compile error and no log line.
# INCLUDE_RCPU shifts soc_cm_device_vectors_t by a pointer; BCM_ALL_CHIPS
# decides which chips' memories are in soc_mem_t.
SDK_DEFINES_FILE = $(SDK_DIR)/lib/openbcm/sdk-defines.txt
ifeq ($(wildcard $(SDK_DEFINES_FILE)),)
$(error $(SDK_DEFINES_FILE) is missing. It is produced by the openbcm package; build that first. Guessing these defines is not a workable fallback -- a wrong set compiles, links, runs and corrupts every struct the SDK shares with us.)
endif
CFLAGS  += $(shell cat $(SDK_DEFINES_FILE))
CFLAGS  += -DNOSAIC_QUERY_DRIVER=\"helix4\" -DNOSAIC_WITH_SDK -I$(SDK_DIR)/include/openbcm
SDK_LIBS = -Wl,--start-group $(wildcard $(SDK_DIR)/lib/openbcm/*.a) -Wl,--end-group
LDLIBS  += $(SDK_LIBS) -lpthread -lm -lrt
OBJS    += ../common/tapbridge.o ../common/l3sync.o ../common/query.o ../common/vlan.o ../common/lag.o ../common/rstp.o
endif

# sdk.o is always built. Without SDK_DIR it compiles to a stub that says the
# binary was built without the SDK, which is a better answer than a link error
# naming a symbol. dmapool is always built too: the BDE divides the reserved
# region whether or not there is a vendor library above it, and it takes a
# mutex to do so, so pthread is a base dependency rather than an SDK one.
OBJS += main.o bde.o sdk.o ../common/props.o ../common/dmapool.o

all: nosd-helix4

nosd-helix4: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

main.o: main.c bde.h sdk.h ../common/props.h
bde.o:  bde.c  bde.h ../common/mmio.h ../common/dmapool.h
sdk.o:  sdk.c  bde.h sdk.h ../common/mmio.h ../common/props.h
../common/props.o: ../common/props.c ../common/props.h
../common/dmapool.o: ../common/dmapool.c ../common/dmapool.h
../common/tapbridge.o: ../common/tapbridge.c ../common/tapbridge.h ../common/vlan.h ../common/lag.h ../common/rstp.h
../common/vlan.o: ../common/vlan.c ../common/vlan.h ../common/tapbridge.h ../common/l3sync.h ../common/lag.h ../common/rstp.h
../common/rstp.o: ../common/rstp.c ../common/rstp.h ../common/lag.h ../common/tapbridge.h
../common/lag.o: ../common/lag.c ../common/lag.h ../common/vlan.h ../common/tapbridge.h ../common/l3sync.h
../common/l3sync.o: ../common/l3sync.c ../common/l3sync.h ../common/props.h
../common/query.o: ../common/query.c ../common/query.h ../common/tapbridge.h ../common/lag.h ../common/rstp.h

strip: nosd-helix4
	$(CROSS_COMPILE)strip -s nosd-helix4

clean:
	rm -f *.o ../common/*.o nosd-helix4

.PHONY: all clean strip
