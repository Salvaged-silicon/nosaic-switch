# nosd-td2p.
#
# CROSS_COMPILE is honoured because the package builder passes it, and ignoring
# it is not a harmless default -- it silently produces a binary linked against
# the build container's glibc rather than the one the image ships. That builds
# cleanly, packages cleanly, and fails on the switch with a symbol version
# error that names neither cause.
#
# The SDK is optional at build time. `make` alone builds the BDE and its probe,
# which is what a contributor without the SDK staged can do and what CI runs;
# SDK_DIR turns on the vector layer that lets the SDK drive the chip. Making it
# unconditional would mean nobody can build this without a 1.8 GB vendor tree.
CC      = $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

OBJS = main.o bde.o props.o

# SDK_DIR points at the staged prefix. The openbcm package puts its headers in
# include/openbcm and its archives in lib/openbcm, so both get that suffix --
# the SDK's own #includes are <sal/types.h>, relative to that directory.
ifneq ($(SDK_DIR),)
# THE SDK'S OWN DEFINES, CAPTURED BY ITS BUILD.
#
# Anything linking against the SDK must compile with the same set. The headers
# are shared, so a define set for the library and not for us silently changes
# struct layouts and enum values -- with no compile error, no link error and no
# log line:
#
#   INCLUDE_RCPU    adds a pointer to soc_cm_device_vectors_t after bus_type,
#                   so every function pointer after it lands one slot off. That
#                   cost a round: soc_cm_device_init returned SOC_E_PARAM from
#                   a macro that returns without saying anything, with every
#                   vector correctly set from this side.
#   BCM_ALL_CHIPS   decides which chips' memories are in soc_mem_t, so every
#                   memory ID and NUM_SOC_MEM differ without it.
#
# This file is written by the openbcm package's own build from the compile line
# the SDK used, so it cannot drift from the libraries. An earlier version of
# this makefile listed the feature flags by hand, which was wrong in kind
# rather than degree: it named fourteen, and the real set is a hundred and
# five.
SDK_DEFINES_FILE = $(SDK_DIR)/lib/openbcm/sdk-defines.txt
ifeq ($(wildcard $(SDK_DEFINES_FILE)),)
$(error $(SDK_DEFINES_FILE) is missing. It is produced by the openbcm package; \
build that first. Guessing these defines is not a workable fallback -- a wrong \
set compiles, links, runs and corrupts every struct the SDK shares with us.)
endif
CFLAGS  += $(shell cat $(SDK_DEFINES_FILE))
CFLAGS  += -DNOSAIC_WITH_SDK -I$(SDK_DIR)/include/openbcm
# The SDK is a set of static archives with circular references between them,
# so they go inside --start-group; listing them once in dependency order is a
# problem nobody has solved and does not need solving.
SDK_LIBS = -Wl,--start-group $(wildcard $(SDK_DIR)/lib/openbcm/*.a) -Wl,--end-group
LDLIBS  += $(SDK_LIBS) -lpthread -lm -lrt
OBJS    += sdk.o tapbridge.o l3sync.o led.o
endif

nosd-td2p: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

# Stripping is a separate target rather than -s at link time, so a debug build
# is one target away rather than a makefile edit.
#
# It is not optional in practice when the SDK is linked in: the archives carry
# enough symbols and debug info to make the binary 464 MB, against a 768 MB
# slot that also has to hold a kernel, a root filesystem and room for the other
# A/B slot's data. Stripped it is a fraction of that.
strip: nosd-td2p
	$(CROSS_COMPILE)strip $<
	@echo "    stripped to $$(stat -c%s nosd-td2p) bytes"

main.o: main.c bde.h sdk.h props.h
bde.o: bde.c bde.h
sdk.o: sdk.c bde.h sdk.h props.h
tapbridge.o: tapbridge.c tapbridge.h
l3sync.o: l3sync.c l3sync.h props.h
props.o: props.c props.h

clean:
	rm -f nosd-td2p *.o

.PHONY: clean strip
