#!/bin/sh
# Generate this board's SDK port map and PHY wiring from YOUR OWN switch.
#
# WHY THIS EXISTS
#
# The SDK cannot bring up a single port without knowing which logical port is
# wired to which physical SerDes lane, and on this board it also cannot reach a
# front-panel port at all without knowing which MDIO address that port's
# external PHY answers on. Both are facts about the board, and no public
# document publishes them for this machine.
#
# So they have to be read from the vendor's software on the switch that has it.
# That makes the result vendor-derived, which is why NOSaic ships this generator
# and not its output. You run it once against your own switch and the map stays
# yours.
#
# WHY GUESSING DOES NOT WORK
#
# The port map is not an offset, and a sequential one satisfies every bandwidth
# rule the chip enforces while reaching none of the right cages -- the failure
# looks like working.
#
# The copper case is worse. port_phy_addr_<n> says which MDIO address each of
# the 48 BCM84848s answers on. Get it wrong and the SDK talks to a real PHY that
# belongs to a different socket, so a port reports the link state of another
# port. Nothing errors.
#
# WHAT IT EMITS
#
# Everything keyed on a port number that describes wiring: portmap_<n>,
# port_phy_addr_<n>, phy_mdi_pair_map_<n>, phy_long_xfi_<n>, phy_fiber_pref_<n>,
# phy_an_c73_<n>, phy_an_c37_<n>, port_init_autoneg_<n>.
#
# NOT phy_bus_i2c_<n>, despite it being port-keyed. It appears nowhere in the
# vendor's configuration -- it is ours, added to put the external PHYs on the
# SDK's MDIO bus, and it is the same value for all 48 copper ports. Board data
# is what the vendor knows about this unit; a uniform switch we turned on is
# not that, so it ships in asic.conf instead.
#
# SerDes polarity goes to polarity.conf instead -- see mkpolarity.sh. Two files
# rather than three because those are the two the datapath already loads, and
# one capture feeds both.
#
# READ-ONLY. `platform trident diag config` is a show command. Nothing is
# written to the switch, which may be carrying traffic while this runs.
#
# USAGE
#   ./mkportmap.sh --stdin < captured.txt > portmap.conf
#   ./mkportmap.sh <switch-ip>            > portmap.conf
#
# The --stdin form takes the command's output from anywhere -- a serial console
# session, for instance, which is how it is done on a switch with no management
# address. On the switch, under EOS:
#
#   localhost# enable
#   localhost# platform trident diag config
#
# Environment: SW_USER (default admin), SW_PW (default arista).
#              QSFP_BREAKOUT=1 keeps each QSFP+ cage as four 10G ports for a
#              61-port build. The default is one 40G port per cage, 52 ports,
#              which is what this board's asic.conf is written for.
set -u

SW_USER="${SW_USER:-admin}"
SW_PW="${SW_PW:-arista}"
QSFP_BREAKOUT="${QSFP_BREAKOUT:-0}"

# ⚠ THE UNIT SUFFIX IS NOT PART OF THE NAME, AND IT HAS TO BE REWRITTEN.
#
# EOS drives this chip as unit 1, so every property it prints is tagged `.1`.
# NOSaic's datapath gets unit 0 from soc_cm_device_create, and a property tagged
# for a unit it does not have is simply never read -- no error, no warning, and
# a port map that appears to have been loaded and was not.
#
# Only the FINAL component is a unit. In `portmap_46.1` the 46 is part of the
# name and the 1 is the unit.
UNIT_FROM="${UNIT_FROM:-1}"
UNIT_TO="${UNIT_TO:-0}"

# ⚠ THE CAPTURE IS A 61-PORT BOARD AND THIS ONE IS 52.
#
# EOS runs the four QSFP+ cages broken out as four 10G lanes each, so its map
# has 61 ports: 48 copper plus 13. NOSaic runs them as one 40G port per cage --
# 52 ports, matching pbmp_xport_xe in asic.conf. Emitting the capture unchanged
# hands the SDK a port count its own bitmap disagrees with.
#
# The cages start at 49 and each owns four consecutive logical ports, so the
# first lane of each is 49, 53, 57 and 61. Those survive, the other nine are
# dropped, and the survivor's speed becomes :40.
#
# phy_an_c37_ and phy_an_c73_ are the exception and keep every lane. That is
# read off this board's own working 52-port configuration rather than reasoned
# about: the SDK is the only authority on which of its properties are per-lane,
# and it keeps all thirteen of those.
LANE_FILTER='
{
    line = $0
    if (match(line, /^[a-z0-9_]+_[0-9]+\./) == 0) { print line; next }

    eq   = index(line, "=")
    name = substr(line, 1, eq - 1)
    dot  = length(name); while (dot > 0 && substr(name, dot, 1) != ".") dot--
    stem = substr(name, 1, dot - 1)
    u    = length(stem); while (u > 0 && substr(stem, u, 1) != "_") u--
    port = substr(stem, u + 1) + 0
    fam  = substr(stem, 1, u)

    if (port <= 48)          { print line; next }
    if (breakout == "1")     { print line; next }
    if (fam == "phy_an_c37_" || fam == "phy_an_c73_") { print line; next }
    if ((port - 49) % 4 != 0) { next }

    if (fam == "portmap_") { sub(/:10$/, ":40", line) }
    print line
}'

usage() {
	echo "usage: $0 --stdin < captured.txt > portmap.conf" >&2
	echo "       $0 <switch-ip> > portmap.conf" >&2
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
	grep -E "^(portmap_[0-9]+|port_phy_addr_[0-9]+|phy_mdi_pair_map_[0-9]+|phy_long_xfi_[0-9]+|phy_fiber_pref_[0-9]+|phy_an_c73_[0-9]+|phy_an_c37_[0-9]+|port_init_autoneg_[0-9]+)(\.[0-9]+)?=" |
	sed "s/\.${UNIT_FROM}=/.${UNIT_TO}=/" |
	awk -v breakout="$QSFP_BREAKOUT" "$LANE_FILTER" |
	sort)

printf '# Generated by mkportmap.sh from this switch. Do not commit.\n'
printf '# Port map and PHY wiring for one physical board.\n\n'
printf '%s\n' "$props"

# Counts go to stderr so they cannot land in the file. They are worth printing:
# a map with fewer entries than the board has ports is exactly the failure this
# generator exists to make visible, and it is silent everywhere else.
maps=$(printf '%s\n' "$props" | grep -c '^portmap_' || true)
phys=$(printf '%s\n' "$props" | grep -c '^port_phy_addr_' || true)
want=52; [ "$QSFP_BREAKOUT" = "1" ] && want=61
echo "$0: $maps port map entries, $phys PHY addresses" >&2
[ "$maps" -eq "$want" ] || echo "$0: WARNING: expected $want port map entries, got $maps" >&2
[ "$phys" -ge 48 ] || echo "$0: WARNING: 48 ports are copper and only $phys have a PHY address" >&2
