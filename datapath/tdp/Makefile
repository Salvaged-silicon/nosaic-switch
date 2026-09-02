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

# The SDK is optional at build time. `make` alone builds the BDE and its probe,
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
CFLAGS  += -DNOSAIC_WITH_SDK -I$(SDK_DIR)/include/openbcm
SDK_LIBS = -Wl,--start-group $(wildcard $(SDK_DIR)/lib/openbcm/*.a) -Wl,--end-group
LDLIBS  += $(SDK_LIBS) -lpthread -lm -lrt
endif

# sdk.o is always built. Without SDK_DIR it compiles to a stub that says the
# binary was built without the SDK, which is a better answer than a link error
# naming a symbol -- and it keeps the probe's argument handling identical
# whether or not the vendor tree was staged.
OBJS += probe.o bde.o sdk.o

all: tdp-probe

tdp-probe: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

probe.o: probe.c bde.h sdk.h ../common/mmio.h
bde.o:   bde.c   bde.h ../common/mmio.h
sdk.o:   sdk.c   bde.h sdk.h ../common/mmio.h

strip: tdp-probe
	$(CROSS_COMPILE)strip -s tdp-probe

clean:
	rm -f *.o tdp-probe

.PHONY: all clean strip
