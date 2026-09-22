#!/bin/sh
# Map this board's i2c mux channels to the devices behind them, from a switch
# running NOSaic.
#
# WHY THIS EXISTS
#
# Every environmental on this board -- the four temperature sensors, the fan
# controller, both power supplies and the SPROM carrying the thermal
# thresholds -- sits behind a single i2c mux at 0x70 on the PCH's i801 SMBus,
# selected per access. Which channel holds which device is not published, is
# not in the vendor's configuration, and cannot be inferred: the vendor's own
# software asks its board controller rather than addressing the parts.
#
# Without that table there is no platform HAL for this board, so NOSaic reads
# no sensors and drives no fans.
#
# ⚠ RUN THIS UNDER A NETBOOT, AND NOWHERE ELSE.
#
# This is the opposite of the port-map generator, which reads the vendor's OS.
# Here the vendor's OS is the problem: NX-OS has i2cdetect and deliberately no
# /dev/i2c-* nodes, because its own klm_cctrli owns the bus. Probing it there
# means two masters on one bus while the switch is managing a failed power
# supply, for a table you can get for free the moment our own kernel is
# running.
#
#   make netboot BOARD=cisco-n3172tq
#
# A netboot leaves the disk untouched and gives us the bus with nothing else on
# it. See docs/install.md.
#
# WHAT IT DOES TO THE SWITCH
#
# It writes one byte to the mux, repeatedly: the channel select. That is the
# only write, it is what the mux exists for, and the mux is left deselected on
# exit even if the scan is interrupted.
#
# Scanning is done with SMBus read-byte, not quick-write. A quick-write probe
# is a write to every address on the bus, which on a bus carrying a fan
# controller and two power supplies is not a read-only operation however it is
# described.
#
# USAGE
#   ./mki2cmap.sh              scan every channel and print the table
#   ./mki2cmap.sh --yaml       also print a candidate board.yml block
#
# Environment: MUX (default 0x70), CHANNELS (default 8), BUS (default: found by
#              adapter name).
set -u

MUX="${MUX:-0x70}"
CHANNELS="${CHANNELS:-8}"

for t in i2cdetect i2cset; do
	command -v "$t" >/dev/null 2>&1 || {
		echo "$0: $t is missing. It is a busybox applet in every NOSaic image;" >&2
		echo "        if it is absent you are not running one." >&2
		exit 1
	}
done

# The adapter, found by name rather than by number.
#
# ⚠ ADAPTER NUMBERS COME FROM PROBE ORDER. On this board there is only one
# today, but the mux registers more the moment its driver binds -- each channel
# becomes its own adapter -- so a hardcoded 0 stops meaning what it meant. The
# i801 is the parent, and it is the one to drive by hand.
# ⚠ ANCHORED ON "i2c-N", BECAUSE "i2c" CONTAINS A DIGIT.
#
# The obvious "first number on the line" reads the 2 out of i2c-0 and sends
# every subsequent probe at bus 2 -- which on this board is a mux channel or
# nothing at all, so the scan comes back empty and the board looks like it has
# no devices. Caught by running this against a fake i2cdetect rather than
# against a switch.
find_bus() {
	i2cdetect -l 2>/dev/null |
		grep -iE "i801" |
		sed -n 's/^i2c-\([0-9][0-9]*\).*/\1/p' |
		head -1
}

BUS="${BUS:-$(find_bus)}"
if [ -z "$BUS" ]; then
	echo "$0: no i801 SMBus adapter found. i2cdetect -l says:" >&2
	i2cdetect -l >&2
	echo "" >&2
	echo "        Without it there is no platform bus at all -- check that the" >&2
	echo "        kernel has CONFIG_I2C_I801 and CONFIG_I2C_CHARDEV." >&2
	exit 1
fi
echo "$0: using i2c bus $BUS" >&2

# Leave the mux deselected however this exits. A mux left pointing at a
# channel is a bus that answers for one device and hides the rest, and the next
# person to scan it gets a different answer for no visible reason.
cleanup() { i2cset -y "$BUS" "$MUX" 0x00 2>/dev/null || true; }
trap cleanup EXIT INT TERM

# ⚠ "UU" IS A DEVICE, NOT AN EMPTY ADDRESS.
#
# i2cdetect prints the address where it found something it could probe, and
# "UU" where it found something it would not probe because a driver is already
# bound to it. Both are devices. A parser that keeps only the hex cells reports
# a channel whose sensor already has a driver as empty -- which is the one
# reading that would make somebody stop looking.
#
# Cell i occupies columns 5+3i..6+3i, which is i2c-tools' fixed layout. Taken
# as fixed rather than read off the header row: the 00: row is indented past
# its first three cells, so anchoring on the header labels puts every cell one
# column left of where it actually is.
SCAN_AWK='
NR == 1 { next }
/^[0-9a-f][0-9a-f]:/ {
    base = strtonum("0x" substr($0, 1, 2))
    for (i = 0; i < 16; i++) {
        cell = substr($0, 5 + 3 * i, 2)
        if (cell == "--" || cell ~ /^ *$/) continue
        printf "%02x %s\n", base + i, (cell == "UU" ? "driver-bound" : "free")
    }
}'

scan() {
	# Read-byte probing (-r), never quick-write. See the note above.
	i2cdetect -y -r "$BUS" 2>/dev/null | awk "$SCAN_AWK" | sort -u
}

# A guess at what an address usually is on this class of board, printed as a
# hint and labelled as one. Identification is reading the part's own ID
# register, which needs a driver bound; this is only enough to know which
# channel to look at first.
hint() {
	case "$1" in
		48|49|4a|4b|4c|4d|4e|4f) echo "temperature sensor (LM75/TMP10x family address range)" ;;
		50|51|52|53|54|55|56|57) echo "EEPROM -- the SPROM carrying the thermal thresholds is one of these" ;;
		58|59|5a|5b)             echo "PMBus device -- a power supply reports voltage, current and its own fan here" ;;
		2c|2d|2e|2f)             echo "fan controller or hardware monitor" ;;
		70|71|72|73|74|75|76|77) echo "an i2c mux -- possibly a second one behind the first" ;;
		*)                       echo "" ;;
	esac
}

echo ""
echo "# Root bus, mux deselected"
echo "# ------------------------"
cleanup
root=$(scan)
if [ -z "$root" ]; then
	echo "# nothing answered. That is not expected: the mux itself should."
else
	printf '%s\n' "$root" | while read -r a state; do
		[ -n "$a" ] || continue
		note=$(hint "$a")
		[ "$state" = "driver-bound" ] && note="${note:+$note, }a driver is already bound"
		printf "#   0x%s  %s\n" "$a" "$note"
	done
fi

case " $(printf '%s\n' "$root" | awk '{print $1}' | tr '\n' ' ') " in
	*" ${MUX#0x} "*) : ;;
	*)
		echo ""
		echo "$0: WARNING: the mux did not answer at $MUX." >&2
		echo "        Everything below will be the root bus repeated, which is not a map." >&2
		;;
esac

echo ""
echo "# Behind the mux at $MUX"
echo "# ---------------------"
ch=0
found=0
while [ "$ch" -lt "$CHANNELS" ]; do
	# One bit per channel, which is how a PCA954x-style mux selects. Written
	# as hex so the value in an error message is the value somebody would type.
	sel=$(printf '0x%02x' $((1 << ch)))
	if ! i2cset -y "$BUS" "$MUX" "$sel" 2>/dev/null; then
		echo "# channel $ch: cannot select (mux refused the write)"
		ch=$((ch + 1))
		continue
	fi
	devs=$(scan)
	# The mux answers on every channel because it is upstream of all of them.
	devs=$(printf '%s\n' "$devs" | grep -v "^${MUX#0x} " || true)
	if [ -z "$devs" ]; then
		# Expected on a chassis with empty sockets: this one has no optics
		# fitted and a dead PSU 1, so at least two channels legitimately have
		# nothing on them.
		echo "# channel $ch: empty"
	else
		printf '%s\n' "$devs" | while read -r a state; do
			[ -n "$a" ] || continue
			note=$(hint "$a")
			[ "$state" = "driver-bound" ] && note="${note:+$note, }a driver is already bound"
			printf "# channel %d: 0x%s  %s\n" "$ch" "$a" "$note"
		done
		found=$((found + $(printf '%s\n' "$devs" | grep -c . || true)))
	fi
	ch=$((ch + 1))
done
cleanup

echo ""
echo "$0: $found device(s) found behind the mux" >&2
[ "$found" -gt 0 ] || {
	echo "$0: WARNING: nothing behind the mux at all. Either the channel select" >&2
	echo "        is not a bitmask on this part, or the kernel's own mux driver" >&2
	echo "        has claimed 0x70 and is arbitrating against these writes --" >&2
	echo "        check i2cdetect -l for per-channel adapters and scan those" >&2
	echo "        instead." >&2
}

if [ "${1:-}" = "--yaml" ]; then
	cat <<'YAML'

# ── Candidate board.yml block ───────────────────────────────────────────────
#
# ⚠ ADDRESSES ONLY. The part names below are the hints above, not
# identifications, and a wrong part name is worse than none: the driver binds,
# the sensor appears, and it reports a number read from the wrong register.
# Confirm each one by binding its driver and checking the reading is sane
# against `show environment temperature` on the vendor OS -- the lab chassis
# idles at ASIC 56 C, D1 38 C, D2 37 C, D3 31 C.
#
# ⚠ AND THIS SHAPE DOES NOT EXIST ON THIS BRANCH YET. platform_hal.i2c and the
# Linux-i2c HAL behind it were written for the Edgecore AS4610 and live on
# board/edgecore-as4610-54t (internal/platformhal/i2cmap.go). Two boards now
# want it, which is the argument for landing it on main. Until it does, this
# block is a record of what was measured rather than something board.yml will
# accept.
#
# platform_hal:
#   driver: cisco-qz2
#   i2c:
#     adapter: "i801"
#     mux: {bus: 0, addr: 0x70}
#     sensors:
#       - {name: front-left,  part: <confirm>, bus: <channel>, addr: 0x<addr>}
#       - {name: front-right, part: <confirm>, bus: <channel>, addr: 0x<addr>}
#       - {name: back,        part: <confirm>, bus: <channel>, addr: 0x<addr>}
#     eeprom: {bus: <channel>, addr: 0x<addr>}
#
# The ASIC die sensor is NOT on this bus. The vendor reads it through the
# switch chip rather than from the board controller, and it is the hottest
# thing on the box -- so it belongs to the datapath, not to the platform HAL.
YAML
fi
