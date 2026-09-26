#!/bin/sh
# Run the 41 blocks the link layer needs, in order, against the SCD local bus.
#
# ⚠ THE ORDER IS NOT MINE AND MUST NOT BE TIDIED. It is the order the vendor
# sequence's own splice points imply, and blocks here drive hardware through
# states where the intermediate values are the point -- collapsing or
# reordering EPL's wedges the chip.
#
# ⚠ AND IT ONLY EVER RUNS THIS LIST. The directory also builds probes and
# tools that do not take the same arguments and fall through to writing the
# chip; the prior investigation wedged a switch hard enough to need a power
# cycle by running the lot.
set -e
cd "$(dirname "$0")"
BDF=${BDF:-0000:04:00.0}          # the SCD, not the FM6000: see fetch.sh

ORDER="cminit safinit ffuinit l2linit
parserinit modinit eplseq l2arseq
l2arpre l2arinit mapperpre mgmt2pre
hashinit cmwm mapper smalltables
cmrest parserfields esched modports
erl sweeperinit cmminit monitorinit
statsarinit eaclinit laginit glortinit
tbl3init crmdrop l3arinit l3arslice1
l3arslice4 l3arslice3 l3arslice2 l3artables
sweepinit mgmt2init eplinit mapperinit
ffubstinit"

# ⚠ THE SCAFFOLD HAS NO GUARD. datapath/fm6000 refuses the addresses that are
# measured to take this chip off the bus -- the ECC banks, 0x240036,
# 0x260014, the EPL block before boot. These blocks map the BAR themselves
# and go straight at it, so none of that applies. The first run of all 41
# left the chip dead with every block reporting success.
#
# So liveness is checked after every block and the run stops at the one that
# did it. A sequence that reports "ran 41, nonzero 0" over a corpse is worse
# than a failure, because it reads like success.
D=/sys/bus/pci/devices/0000:04:00.0/resource
B1=$(sed -n 2p $D | cut -d' ' -f1)
alive() { [ "$(busybox devmem $((B1 + 0x1c021 * 4)) 32)" = "0x00000208" ]; }

alive || { echo "chip is already dead before we start -- pulse and boot first"; exit 1; }

ran=0; bad=0; missing=0
for b in $ORDER; do
	if [ ! -x "./fm6000_$b" ]; then
		echo "  MISSING  $b"; missing=$((missing + 1)); continue
	fi
	# ⚠ Two argument conventions. Older blocks take the BDF positionally;
	# ones written later want -b and answer a bare one with usage and exit
	# 2. The prior investigation had 14 of 41 silently no-op on exactly
	# this -- a block that "ran" and did nothing -- so the flag form is
	# tried first and the bare form only on exit 2.
	if ./fm6000_$b -b "$BDF" >/dev/null 2>&1; then
		ran=$((ran + 1))
	elif [ $? -eq 2 ] && ./fm6000_$b "$BDF" >/dev/null 2>&1; then
		ran=$((ran + 1))
	else
		echo "  nonzero  $b"; bad=$((bad + 1))
	fi
	if ! alive; then
		echo "  *** $b TOOK THE CHIP OFF THE BUS -- stopping here ***"
		echo "ran $ran before that, of 41"
		exit 2
	fi
done
echo "ran $ran, nonzero $bad, missing $missing (of 41)"
