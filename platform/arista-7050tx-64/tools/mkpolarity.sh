#!/bin/sh
# Generate this board's SerDes polarity table from YOUR OWN switch.
#
# WHY THIS EXISTS
#
# A PCB can invert the two halves of a differential pair to make a cleaner
# layout, and where it has, the SDK has to invert them back. Which lanes are
# flipped is a fact about this board's copper, published nowhere for this
# machine, and read from the vendor's software on the switch that has it.
# Vendor-derived, so NOSaic ships the generator and not its output.
#
# The same capture feeds mkportmap.sh. Two files rather than one because those
# are the two the datapath already loads:
#
#   ./mkportmap.sh  --stdin < captured.txt > portmap.conf
#   ./mkpolarity.sh --stdin < captured.txt > polarity.conf
#
# WHAT A WRONG TABLE LOOKS LIKE
#
# Not a dead port. A flipped pair the SDK does not know about still trains --
# the link comes up, and then errors on traffic, or negotiates a lower speed
# than it should. It reads as a marginal cable rather than as configuration.
#
# READ-ONLY. `platform trident diag config` is a show command.
#
# USAGE
#   ./mkpolarity.sh --stdin < captured.txt > polarity.conf
#   ./mkpolarity.sh <switch-ip>            > polarity.conf
#
# Environment: SW_USER (default admin), SW_PW (default arista).
#              QSFP_BREAKOUT=1 emits per-lane values for a 61-port build.
set -u

SW_USER="${SW_USER:-admin}"
SW_PW="${SW_PW:-arista}"
QSFP_BREAKOUT="${QSFP_BREAKOUT:-0}"

# EOS drives this chip as unit 1 and NOSaic's datapath gets unit 0. A property
# tagged for a unit we do not have is never read, with no error -- see the
# longer note in mkportmap.sh.
UNIT_FROM="${UNIT_FROM:-1}"
UNIT_TO="${UNIT_TO:-0}"

# ⚠ A 40G PORT'S POLARITY IS A BITMASK OVER ITS FOUR LANES, NOT ITS FIRST LANE.
#
# This is the part that punishes a reasonable guess. The capture is a 61-port
# board -- each QSFP+ cage broken out as four 10G lanes, each with its own
# 0x0 or 0x1 -- and this board runs them as one 40G port per cage. Collapsing
# that by keeping the first lane, which is what the port map does, is wrong
# here: the surviving property covers all four lanes and each lane is one bit.
#
# Read off this board's own working configuration:
#
#   capture   flip_57=0x1  flip_58=0x0  flip_59=0x1  flip_60=0x1
#   correct   flip_57=0xd                                  (binary 1101)
#
# Keeping the first lane would have written 0x1 and left three lanes
# un-inverted -- a 40G port that trains and then errors, which is the failure
# mode described above. Bit 0 is the cage's first lane.
LANE_FILTER='
function hex(s,   i, c, d, n) {
    sub(/^0[xX]/, "", s)
    n = 0
    for (i = 1; i <= length(s); i++) {
        c = tolower(substr(s, i, 1))
        d = index("0123456789abcdef", c) - 1
        if (d < 0) return 0
        n = n * 16 + d
    }
    return n
}
{
    line = $0
    eq   = index(line, "=")
    name = substr(line, 1, eq - 1)
    val  = substr(line, eq + 1)
    dot  = length(name); while (dot > 0 && substr(name, dot, 1) != ".") dot--
    stem = substr(name, 1, dot - 1)
    unit = substr(name, dot + 1)
    u    = length(stem); while (u > 0 && substr(stem, u, 1) != "_") u--
    port = substr(stem, u + 1) + 0
    fam  = substr(stem, 1, u)

    if (port <= 48 || breakout == "1") { print line; next }

    # Accumulate the cage. Each lane contributes a distinct bit, so adding is
    # the same as or-ing and needs no bitwise operator -- which POSIX awk
    # does not have.
    base = 49 + int((port - 49) / 4) * 4
    key  = fam SUBSEP base SUBSEP unit
    if (hex(val) != 0) mask[key] += 2 ^ ((port - 49) % 4)
    lanes[key]++
    first[key] = (port == base) ? val : first[key]
}
END {
    for (k in lanes) {
        split(k, p, SUBSEP)
        # ⚠ A cage that appears ONCE is already a 40G port and its value is
        # already the mask. Not every cage is broken out: in the capture from
        # this board, cages 1-3 are four 10G lanes each and cage 4 is a single
        # 40G port carrying 0xf. Accumulating that one turns 0xf into 0x1 and
        # silently un-inverts three lanes.
        if (lanes[k] == 1) printf "%s%d.%s=%s\n",   p[1], p[2], p[3], first[k]
        else               printf "%s%d.%s=0x%x\n", p[1], p[2], p[3], mask[k]
    }
}'

usage() {
	echo "usage: $0 --stdin < captured.txt > polarity.conf" >&2
	echo "       $0 <switch-ip> > polarity.conf" >&2
	exit 2
}

[ $# -eq 1 ] || usage

if [ "$1" = "--stdin" ]; then
	src=$(cat)
else
	command -v sshpass >/dev/null 2>&1 || {
		echo "$0: sshpass is not installed; capture the output by hand and use --stdin" >&2
		exit 1
	}
	src=$(sshpass -p "$SW_PW" ssh -o StrictHostKeyChecking=no \
		"$SW_USER@$1" 'enable
platform trident diag config' 2>/dev/null) || {
		echo "$0: could not read the configuration from $1" >&2
		exit 1
	}
fi

[ -n "$src" ] || { echo "$0: no input" >&2; exit 1; }

props=$(printf '%s\n' "$src" |
	sed 's/^[[:space:]]*//' |
	grep -E "^phy_xaui_(tx|rx)_polarity_flip_[0-9]+(\.[0-9]+)?=" |
	sed "s/\.${UNIT_FROM}=/.${UNIT_TO}=/" |
	awk -v breakout="$QSFP_BREAKOUT" "$LANE_FILTER" |
	sort)

printf '# Generated by mkpolarity.sh from this switch. Do not commit.\n'
printf '# SerDes lane polarity for one physical board.\n\n'
printf '%s\n' "$props"

n=$(printf '%s\n' "$props" | grep -c '^phy_xaui_' || true)
want=104; [ "$QSFP_BREAKOUT" = "1" ] && want=122
echo "$0: emitted $n polarity entries" >&2
[ "$n" -eq "$want" ] || echo "$0: WARNING: expected $want entries, got $n" >&2
