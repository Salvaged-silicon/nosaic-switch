# nosd-td2p. Plain make: this is three files and does not need more.
#
# CROSS_COMPILE is honoured because the package builder passes it, and ignoring
# it is not a harmless default -- it silently produces a binary linked against
# the build container's glibc rather than the one the image ships. That builds
# cleanly, packages cleanly, and fails on the switch with a symbol version
# error that names neither cause.
#
# The SDK is not linked in yet. The BDE is the layer beneath it and is worth
# proving on hardware first, because everything above depends on the mapping
# being right.
CC      = $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

nosd-td2p: main.o bde.o
	$(CC) $(LDFLAGS) -o $@ main.o bde.o

main.o: main.c bde.h
bde.o: bde.c bde.h

clean:
	rm -f nosd-td2p *.o

.PHONY: clean
