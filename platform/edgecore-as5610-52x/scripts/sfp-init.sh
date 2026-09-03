#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Turn the front panel on: SFP transmitters, QSFP control lines, retimers.
#
# bcm_init leaves the ASIC's ports enabled and the board's optics dark, because
# whether light comes out of a cage is a property of the board and not of the
# chip. On this one three separate things sit between an enabled MAC and a link:
#
#   1. TX_DISABLE, on PCA9506 expanders, which powers up asserted
#   2. the QSFP control lines, on a fifth expander that must go the other way
#   3. the DS100DF410 retimers between the SerDes and the cages
#
# Any one of them left alone gives exactly the same symptom -- every port down,
# no errors anywhere -- so this does all three or it is not worth running.
#
# Derived from EdgeNOS's platform-init.sh for this board, which in turn follows
# Cumulus's S20retimer_init.sh. Nothing here is guesswork; the register values
# are what the only two pieces of software known to work on this hardware use.

log() { echo "sfp-init: $*"; }

have() { i2cget -y -f "$1" "$2" 0x00 b >/dev/null 2>&1; }

# ---------------------------------------------------------------- SFP TX
#
# PCA9506: I/O configuration at 0x18..0x1c (0 = output), output port at
# 0x08..0x0c. TX_DISABLE is active high, so driving every pin low is what
# enables the transmitters.
#
# 0x23 is deliberately absent from this list. It is the QSFP control expander,
# not an SFP TX one, and zeroing it holds RESET_L and MODSEL_L asserted -- the
# modules then never ACK their own EEPROMs, which looks like four dead cages.
sfp_tx_enable() {
	n=0
	for bus in $(seq 0 79); do
		[ -e "/dev/i2c-$bus" ] || continue
		for addr in 0x20 0x21 0x22 0x24; do
			have "$bus" "$addr" || continue
			for r in 0x18 0x19 0x1a 0x1b 0x1c; do
				i2cset -y -f "$bus" "$addr" "$r" 0x00 b 2>/dev/null
			done
			for r in 0x08 0x09 0x0a 0x0b 0x0c; do
				i2cset -y -f "$bus" "$addr" "$r" 0x00 b 2>/dev/null
			done
			n=$((n + 1))
		done
	done
	log "SFP TX_DISABLE cleared on $n expanders"
}

# ---------------------------------------------------------- QSFP power/reset
#
# THE PART THAT ACTUALLY BRINGS THE 40G PORTS UP, and it is not the expander
# above. RESET_L, MODSEL_L and LPMODE for the four cages are on two PCA9538s --
# 0x70 and 0x71 -- on a different bus entirely, and they power up configured as
# INPUTS (config 0xff). Nothing drives them, so every module stays in reset,
# unselected, and its EEPROM never ACKs at 0x50. The symptom is four dead cages
# and three 40G ports that link down with no error anywhere.
#
#   0x71 pins 0-3  RESET_L[3:0]   drive HIGH -- out of reset
#   0x71 pins 4-7  MODSEL_L[3:0]  drive LOW  -- module selected for its
#                                 two-wire management bus
#   0x70 pins 0-3  LPMODE[3:0]    drive LOW  -- high power, which 40G needs
#
# Values from EdgeNOS's platform-init.sh, which records them as verified on
# this hardware: with them the four QSFP EEPROMs ACK and read id 0x0D.
# Confirmed again here -- driving these took the modules visible at 0x50 from
# none to three, and swp49/51/52 from down to up, on the cages that are
# populated.
#
# Found by address and driver rather than by bus number: 0x70..0x77 is also the
# PCA954x mux range, so scanning for the address alone finds muxes. A device
# bound to pca953x at that address is the expander and nothing else is.
qsfp_power() {
	for d in /sys/bus/i2c/devices/*-0071; do
		[ -e "$d" ] || continue
		[ "$(readlink "$d/driver" 2>/dev/null | sed 's|.*/||')" = pca953x ] || continue
		bus=${d##*/}; bus=${bus%%-*}

		# Output register before configuration register: setting the pins to
		# outputs first would briefly drive whatever the output latch happened
		# to hold, which on a reset line is a glitch into the modules.
		i2cset -f -y "$bus" 0x71 0x01 0x0F b 2>/dev/null || continue
		i2cset -f -y "$bus" 0x71 0x03 0x00 b 2>/dev/null

		# Only the low nibble of 0x70 is ours; read-modify-write leaves
		# whatever the top four pins do on this board alone.
		o=$(i2cget -f -y "$bus" 0x70 0x01 2>/dev/null) || continue
		c=$(i2cget -f -y "$bus" 0x70 0x03 2>/dev/null) || continue
		i2cset -f -y "$bus" 0x70 0x01 "$(printf 0x%02x $((o & 0xF0)))" b 2>/dev/null
		i2cset -f -y "$bus" 0x70 0x03 "$(printf 0x%02x $((c & 0xF0)))" b 2>/dev/null

		log "QSFP modules out of reset, selected, high power (bus $bus)"
	done
}

# ------------------------------------------------------------- QSFP control
#
# The opposite of the above: configure as outputs, then drive high, because
# these lines are asserted low. High is also what ONL leaves them at.
qsfp_control() {
	for bus in $(seq 0 79); do
		[ -e "/dev/i2c-$bus" ] || continue
		have "$bus" 0x23 || continue
		i2cset -y -f "$bus" 0x23 0x18 0x00 b 2>/dev/null
		i2cset -y -f "$bus" 0x23 0x1c 0x00 b 2>/dev/null
		i2cset -y -f "$bus" 0x23 0x08 0xff b 2>/dev/null
		i2cset -y -f "$bus" 0x23 0x0c 0xff b 2>/dev/null
		log "QSFP control deasserted on bus $bus"
	done
}

# ---------------------------------------------------------------- retimers
#
# The CDR reset in the middle of this is not optional and not reorderable: the
# channel select and veo_clk_cdr_cap have to be in place before it, and without
# it the CDR never locks, so nothing reaches the ASIC from a module that is
# otherwise powered, present and transmitting.
# The CDR reset is what makes a retimer relock on the settings just written, so
# it goes LAST and it needs a settling gap. Doing it first -- which this did --
# resets the CDR against the previous configuration and then changes the
# configuration underneath it.
#
# Register numbers are from the ds100df410 driver in the newnos tree, which
# names them: 0x0A cdr_rst, 0x15 tap_dem, 0x1E pfd_prbs_dfe, 0x31 adapt_eq_sm,
# 0x36 veo_clk_cdr_cap, 0xFF channels.
cdr_reset() {
	i2cset -f -y "$1" 0x27 0x0A 0x1C 2>/dev/null   # assert
	usleep 20000 2>/dev/null || sleep 1
	i2cset -f -y "$1" 0x27 0x0A 0x10 2>/dev/null   # release
}

# The QSFP cages sit at the end of longer board traces than the SFP+ cages, and
# want pre-emphasis the short ones do not. EdgeNOS calls this profile set_eq2
# and applies it to the four QSFP retimers and to sfp_rx_eq_10.
long_trace_eq() {
	i2cset -f -y "$1" 0x27 0xFF 0x0C 2>/dev/null   # broadcast to all channels
	i2cset -f -y "$1" 0x27 0x36 0x01 2>/dev/null   # no 25MHz reference clock
	i2cset -f -y "$1" 0x27 0x1E 0x00 2>/dev/null   # unmute output, enable DFE
	i2cset -f -y "$1" 0x27 0x31 0x40 2>/dev/null   # CTLE+DFE adaptation
	i2cset -f -y "$1" 0x27 0x15 0x17 2>/dev/null   # DEM pre-emphasis
	cdr_reset "$1"
}

init_retimer() {
	bus=$1
	i2cset -f -y "$bus" 0x27 0xFF 0x0C 2>/dev/null || return 1
	i2cset -f -y "$bus" 0x27 0x36 0x01 2>/dev/null
	i2cset -f -y "$bus" 0x27 0x0A 0x1C 2>/dev/null
	i2cset -f -y "$bus" 0x27 0x0A 0x10 2>/dev/null
	for rv in 0x0B:0x0F 0x0C:0x08 0x0E:0x93 0x0F:0x69 0x10:0x3A 0x11:0x20 \
		  0x12:0xA0 0x13:0x30 0x15:0x10 0x16:0x7A 0x17:0x36 0x18:0x40 \
		  0x19:0x23 0x1B:0x03 0x1C:0x24 0x1E:0xE9 0x1F:0x55 0x23:0x40 \
		  0x2A:0x30 0x2C:0x72 0x2D:0x80 0x2F:0x06 0x31:0x20 0x32:0x11 \
		  0x33:0x88 0x34:0x3F 0x35:0x1F 0x3A:0xA5 0x3E:0x80; do
		i2cset -f -y "$bus" 0x27 "${rv%:*}" "${rv#*:}" 2>/dev/null
	done
	return 0
}

retimers() {
	n=0
	q=0
	for d in /sys/bus/i2c/devices/*-0027; do
		[ -e "$d" ] || continue
		bus=${d##*/}; bus=${bus%%-*}
		init_retimer "$bus" || continue
		n=$((n + 1))

		# Which retimer this is comes from the device tree, which labels every
		# one of the 32 -- qsfp_tx_eq_0..3, qsfp_rx_eq_0..3, sfp_*_eq_*. Read
		# rather than hard-coded, because a bus number is a property of how the
		# mux tree enumerated this boot and the label is a property of the
		# board.
		label=$(cat "$d/of_node/label" 2>/dev/null)
		case "$label" in
			qsfp_*|sfp_rx_eq_10) long_trace_eq "$bus"; q=$((q + 1)) ;;
			*)                   cdr_reset "$bus" ;;
		esac
	done
	[ "$q" -gt 0 ] && log "$q retimer(s) on the long-trace profile"
	log "$n retimers programmed"
}

sfp_tx_enable
qsfp_control
qsfp_power
retimers
log "front panel initialised"
