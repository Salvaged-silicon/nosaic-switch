# The scaffold. Builds a runner and the 29 relocated-capture blocks.
#
# ⚠ NO PROFILE LISTS THIS AND NO IMAGE INSTALLS IT. It exists so a lane can
# be brought to link while the real generators are written; see README.md.
CC      ?= $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -g -Wall -Wno-unused-result
SRCS    := $(wildcard fm6000_*.c)
BINS    := $(SRCS:.c=)

all: $(BINS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean
