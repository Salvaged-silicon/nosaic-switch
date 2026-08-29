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

OBJS = main.o bde.o

# SDK_DIR points at the staged prefix. The openbcm package puts its headers in
# include/openbcm and its archives in lib/openbcm, so both get that suffix --
# the SDK's own #includes are <sal/types.h>, relative to that directory.
ifneq ($(SDK_DIR),)
# THESE MUST MATCH WHAT THE SDK LIBRARIES WERE BUILT WITH.
#
# Several SDK feature flags change struct layouts, so a mismatch is an ABI
# mismatch, and it is silent: the code compiles, links and runs, and the
# library reads every field of a struct one slot away from where it was
# written.
#
# INCLUDE_RCPU is the one that bites here. It adds a pointer to
# soc_cm_device_vectors_t (include/soc/cmtypes.h) immediately after bus_type,
# so without it every function pointer from config_var_get onward lands in the
# wrong slot. The symptom is soc_cm_device_init returning SOC_E_PARAM with no
# log line at all, because the check that fails is a macro that returns
# without saying anything -- and every vector looks correctly set from this
# side.
#
# The list comes from _ESW_FEATURE_LIST in the SDK's make/Make.config, which
# make/Make.config:1117 turns into -DINCLUDE_<feature>. If the openbcm recipe's
# build flags change, this has to change with them.
SDK_FEATURES = L3 I2C MEM_SCAN EDITLINE RCPU OOB_RCPU CUSTOMER \
               TEST CHASSIS CINT PTP BFD TCB PSTATS
CFLAGS  += $(foreach f,$(SDK_FEATURES),-DINCLUDE_$(f))
CFLAGS  += -DNOSAIC_WITH_SDK -I$(SDK_DIR)/include/openbcm
# The SDK is a set of static archives with circular references between them,
# so they go inside --start-group; listing them once in dependency order is a
# problem nobody has solved and does not need solving.
SDK_LIBS = -Wl,--start-group $(wildcard $(SDK_DIR)/lib/openbcm/*.a) -Wl,--end-group
LDLIBS  += $(SDK_LIBS) -lpthread -lm -lrt
OBJS    += sdk.o
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

main.o: main.c bde.h sdk.h
bde.o: bde.c bde.h
sdk.o: sdk.c bde.h sdk.h

clean:
	rm -f nosd-td2p *.o

.PHONY: clean strip
