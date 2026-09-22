#!/bin/sh
# Generate this board's SDK port map and PHY wiring from YOUR OWN switch.
#
# WHY THIS EXISTS
#
# The SDK cannot bring up a single port without knowing which logical port is
# wired to which physical SerDes lane, and on this board it also cannot reach a
# front-panel port at all without knowing which MDIO address that port's
# external PHY answers on -- all 54 of them have one. Both are facts about the
# board, and no public document publishes them for this machine.
#
# So they have to be read from the vendor's software on the switch that has it.
# That makes the result vendor-derived, which is why NOSaic ships this
# generator and not its output. You run it once against your own switch and the
# map stays yours.
#
# ⚠ TAKE THE CAPTURE BEFORE YOU INSTALL NOSAIC.
#
# Installing replaces the disk, and the vendor's SDK goes with it. Getting the
# capture afterwards means putting NX-OS back on the box first. This is the one
# irreversible ordering constraint in the whole bring-up.
#
# WHY GUESSING DOES NOT WORK
#
# The port map is not an offset. On this board the physical lane numbering has
# a gap -- copper runs 13-16, then SKIPS 17-20, then 21-24 -- and a sequential
# guess satisfies every bandwidth rule the chip enforces while reaching none of
# the right sockets. The failure looks like working.
#
# The MDIO addresses are worse: they are SWAPPED IN PAIRS. xe0 answers at
# address 1, xe1 at 0, xe2 at 3, xe3 at 2. A wrong address is a real PHY
# belonging to a different socket, so a port reports another port's link state.
# Nothing errors.
#
# WHAT IT EMITS
#
#   portmap_<p>                        logical port -> physical lane : speed
#   port_phy_addr_xe<i>                that port's PHY MDIO address
#   phy_84848_<p>                      the port is behind a BCM84848 (copper)
#   phy_84328_<p>                      the port is behind a BCM84328 (cage)
#   phy_mdi_pair_map_<p>               how the copper pairs land on the RJ45
#   phy_aux_voltage_enable_<p>         cage ports
#   serdes_fiber_pref_<p>              cage ports
#   phy_port_primary_and_offset_xe<i>  cage lane -> its master
#
# NOT the polarity flips or the lane swizzles -- those go to polarity.conf, see
# mkpolarity.sh. Two files rather than one because those are the two the
# datapath loads separately, and one capture feeds both.
#
# NOT the ASIC-wide properties either. Those describe the chip as this MODEL
# configures it rather than as this unit is wired, and they ship committed in
# config/asic.conf.
#
# ⚠ TWO INDEX BASES, AND THEY ARE ONE APART.
#
# Some families are keyed by LOGICAL PORT, counting from 1: portmap_1 is the
# first front-panel port. Others are keyed by the SDK's interface name,
# counting from 0: port_phy_addr_xe0 is that same port. Mixing them shifts the
# whole map by one, which lands every copper port on its neighbour's PHY -- and
# since neighbouring PHYs both answer, nothing errors. The filter below tracks
# which base each family uses; do not add a family to it without checking.
#
# ⚠ THE SHELL TRUNCATES SOME PROPERTY NAMES.
#
# `config show` on this SDK prints names like `pci2eb_overrid`, `phy_autoneg_ti`
# and `phy_bus_` -- cut short, and useless as written. That is why this script
# emits only the families it names above rather than everything it sees: a
# truncated name copied into a configuration file is a property that silently
# never takes effect.
#
# READ-ONLY. `config show` is a show command. Nothing is written to the switch,
# which may be carrying traffic while this runs.
#
# USAGE
#   ./mkportmap.sh --stdin < captured.txt > portmap.conf
#
# Capture it on the switch, with root (see docs/install.md):
#
#   bash-4.2# bcm-sdk-shell
#   bcm-shell.0> config show
#
# There is no <switch-ip> form here, unlike the Arista generators. Reaching
# this shell needs `feature bash-shell` and then the SDK shell, and the
# capture has to come back through the management network namespace -- too many
# steps to do blind over ssh from a script. Paste it into a file.
#
# Environment: QSFP_BREAKOUT=1 keeps every QSFP cage as four 10G ports for a
#              72-port build. The default is one 40G port per cage, 54 ports,
#              which is what this board's asic.conf is written for.
#              UNIT_FROM/UNIT_TO rewrite the unit suffix; see below.
set -u

QSFP_BREAKOUT="${QSFP_BREAKOUT:-0}"

# ⚠ THE UNIT SUFFIX, AND WHY THIS BOARD NEEDS NO REWRITE.
#
# The Arista generator rewrites `.1` to `.0` because EOS drives its chip as
# unit 1 while NOSaic's datapath gets unit 0 from soc_cm_device_create -- and a
# property tagged for a unit that does not exist is never read, with no error
# and no warning.
#
# NX-OS drives this chip as unit 0 already: its prompt is `bcm-shell.0>` and
# its properties are tagged `.0`. So the default here is to rewrite nothing.
# The knobs exist because a capture from a different vendor OS, or from a
# chassis platform driving more than one unit, would need it.
#
# Only the FINAL component is a unit. In `portmap_46.0` the 46 is part of the
# name and the 0 is the unit.
UNIT_FROM="${UNIT_FROM:-}"
UNIT_TO="${UNIT_TO:-0}"

# ⚠ THE CAPTURE IS A 72-PORT BOARD AND THIS ONE IS 54.
#
# NX-OS drives all 72 logical ports: 48 copper plus six cages of four lanes
# each, with the 18 subordinate lanes flagged `:i` in the port map. NOSaic runs
# the cages as one 40G port each -- 54 ports, matching pbmp_xport_xe in
# asic.conf. Emitting the capture unchanged hands the SDK a port count its own
# bitmap disagrees with.
#
# The cages start at 49 and each owns four consecutive logical ports, so the
# first lane of each is 49, 53, 57, 61, 65 and 69. Those survive; the other 18
# are dropped. The masters are already `:40` in the vendor's own map, so unlike
# the Arista capture there is no speed to rewrite.
LANE_FILTER='
{
    line = $0
    eq = index(line, "=")
    if (eq == 0) { next }
    name = substr(line, 1, eq - 1)

    # Strip a trailing unit component, which is only a unit if the name has a
    # keyed index before it.
    stem = name
    dot  = length(stem)
    while (dot > 0 && substr(stem, dot, 1) != ".") dot--
    if (dot > 0) { stem = substr(stem, 1, dot - 1) }

    # The keyed index is the trailing run of digits.
    n = length(stem); idx = ""
    while (n > 0 && substr(stem, n, 1) >= "0" && substr(stem, n, 1) <= "9") {
        idx = substr(stem, n, 1) idx; n--
    }
    if (idx == "") { next }
    fam = substr(stem, 1, n)

    # xe-keyed families count from 0; port-keyed families count from 1.
    # Normalise to a logical port before deciding.
    if (fam ~ /_xe$/) { port = idx + 1 } else { port = idx + 0 }

    # Per-core, not per-port: one entry per group of four lanes, and every
    # group survives a 54-port build because the cores do not go away. These
    # belong to polarity.conf and are filtered out here, but the rule is
    # written down because getting it wrong drops twelve of the eighteen.
    if (fam == "xgxs_tx_lane_map_xe" || fam == "xgxs_rx_lane_map_xe") { next }

    if (port <= 48) { print line; next }

    # ⚠ BREAKOUT IS NOT "KEEP THE CAPTURE AS IT IS".
    #
    # The vendor runs every cage as one 40G port, so its map has the master
    # lane at :40 and the other three flagged :i -- subordinate, part of a
    # higher-speed group rather than ports in their own right. Four
    # independent 10G ports is the opposite: every lane is its own port at
    # :10 and none of them is :i. Emitting the capture unchanged hands the
    # SDK three lanes it will not present as ports and one port still
    # striping across all four.
    if (breakout == "1") {
        if (fam == "portmap_") {
            sub(/:i$/, "", line)
            sub(/:40$/, ":10", line)
        }
        print line
        next
    }

    if ((port - 49) % 4 != 0) { next }
    print line
}'

usage() {
	echo "usage: $0 --stdin < captured.txt > portmap.conf" >&2
	echo "" >&2
	echo "  capture it on the switch with:  bcm-shell.0> config show" >&2
	exit 2
}

[ $# -eq 1 ] && [ "$1" = "--stdin" ] || usage

src=$(cat)
[ -n "$src" ] || { echo "$0: no input" >&2; exit 1; }

# The shell prints its prompt on the same line as the first property and
# indents the rest, so both have to come off before anything matches.
clean=$(printf '%s\n' "$src" |
	sed 's/^[[:space:]]*//; s/^bcm-shell\.[0-9]*>[[:space:]]*//')

if [ -n "$UNIT_FROM" ]; then
	clean=$(printf '%s\n' "$clean" | sed "s/\.${UNIT_FROM}=/.${UNIT_TO}=/")
fi

props=$(printf '%s\n' "$clean" |
	grep -E "^(portmap_[0-9]+|port_phy_addr_xe[0-9]+|phy_84848_[0-9]+|phy_84328_[0-9]+|phy_mdi_pair_map_[0-9]+|phy_aux_voltage_enable_[0-9]+|serdes_fiber_pref_[0-9]+|phy_port_primary_and_offset_xe[0-9]+)(\.[0-9]+)?=" |
	awk -v breakout="$QSFP_BREAKOUT" "$LANE_FILTER" |
	sort)

printf '# Generated by mkportmap.sh from this switch. Do not commit.\n'
printf '# Port map and PHY wiring for one physical Cisco Nexus 3172TQ.\n\n'
printf '%s\n' "$props"

# Counts go to stderr so they cannot land in the file. They are worth printing:
# a map with fewer entries than the board has ports is exactly the failure this
# generator exists to make visible, and it is silent everywhere else.
maps=$(printf '%s\n' "$props" | grep -c '^portmap_' || true)
phys=$(printf '%s\n' "$props" | grep -c '^port_phy_addr_xe' || true)
want=54; [ "$QSFP_BREAKOUT" = "1" ] && want=72
echo "$0: $maps port map entries, $phys PHY addresses" >&2
[ "$maps" -eq "$want" ] || echo "$0: WARNING: expected $want port map entries, got $maps" >&2
[ "$phys" -eq "$want" ] || echo "$0: WARNING: expected $want PHY addresses, got $phys" >&2

# Every port on this board has a PHY, cages included, which is unusual enough
# to check for rather than assume: a capture missing the cage PHYs is a capture
# from a different board.
c84848=$(printf '%s\n' "$props" | grep -c '^phy_84848_' || true)
c84328=$(printf '%s\n' "$props" | grep -c '^phy_84328_' || true)
echo "$0: $c84848 BCM84848 copper PHYs, $c84328 BCM84328 cage PHYs" >&2
[ "$c84848" -eq 48 ] || echo "$0: WARNING: expected 48 copper PHYs, got $c84848" >&2
[ "$c84328" -ge 6 ] || echo "$0: WARNING: expected at least 6 cage PHYs, got $c84328" >&2

# An `:i` line surviving the filter means the lane rule did not fire, and the
# SDK would be handed subordinate lanes the port bitmap does not admit.
if printf '%s\n' "$props" | grep -q '^portmap_.*:i'; then
	echo "$0: WARNING: subordinate QSFP lanes (:i) survived the filter" >&2
fi
