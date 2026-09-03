#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# The AS5610-52X's board controller: cooling, and what the box reports about
# itself.
#
#   platform.sh cool     run the fans (a supervised service)
#   platform.sh status   temperatures, fans, power supplies, LEDs
#
# The board powers up with the fan duty at 31 of 31 and stays there, because
# nothing has ever commanded it otherwise. That is safe and it is full speed on
# a switch that needs about a third of it: measured idle, this board sits at
# 29-37 C against sensor limits of 90-127 C.
#
# This is a shell script and not part of the platform HAL because no Go binary
# runs on this board -- the toolchain has no 32-bit big-endian PowerPC target --
# so the CLI that carries the HAL cannot be shipped here. The controller is a
# few byte registers on the local bus, which shell and devmem reach perfectly
# well.
#
# The CPLD, from the device tree: local bus chip select 1 maps to 0xea000000.
#
#   0x00  version        0x01  psu status      0x03  fan status
#   0x0d  fan duty, 0..31                      0x13/0x15  chassis LEDs
#
# Temperatures come from the max6697 on I2C, which binds on its own and gives
# seven sensors; the CPLD has none.

CPLD=0xea000000
CPLD_VERSION=0xea000000
CPLD_PSU2=0xea000001
CPLD_PSU1=0xea000002
CPLD_FAN_STATUS=0xea000003
CPLD_PWM=0xea00000d
CPLD_LED_SYS=0xea000013
CPLD_LED_LOC=0xea000015

# Duty is five bits. The floor is not zero and not a computed number: a
# controller that can be told to stop the fans is one that will eventually be
# told to stop them by a bug. 10/31 is where EdgeNOS idles this same board, so
# the quiet case here is a value that has already run on this hardware.
PWM_MIN=10
PWM_MAX=31

# The curve, in degrees. Below LOW the fans idle; above HIGH they are flat out;
# between, the duty is interpolated.
T_LOW=40
T_HIGH=60

# Rising is immediate and falling is limited to one step a pass, because
# reacting instantly downward makes the fans hunt around a threshold while
# reacting slowly upward is a thermal risk. They are not symmetric problems.
STEP_DOWN=1
INTERVAL=5

log() { echo "cooling: $*"; }

hottest() {
	max=0
	for f in /sys/class/hwmon/hwmon*/temp*_input; do
		[ -r "$f" ] || continue
		v=$(cat "$f" 2>/dev/null) || continue
		v=$((v / 1000))
		[ "$v" -gt "$max" ] && max=$v
	done
	echo "$max"
}

want_pwm() {
	t=$1
	if [ "$t" -le "$T_LOW" ]; then
		echo "$PWM_MIN"
	elif [ "$t" -ge "$T_HIGH" ]; then
		echo "$PWM_MAX"
	else
		span=$((T_HIGH - T_LOW))
		echo $((PWM_MIN + (t - T_LOW) * (PWM_MAX - PWM_MIN) / span))
	fi
}

cool() {
cur=$(devmem $CPLD_PWM 8 2>/dev/null) || {
	log "cannot read the fan register at $CPLD_PWM; not touching the fans"
	exit 1
}
cur=$((cur & PWM_MAX))
log "started; fan duty is $cur/$PWM_MAX"

while :; do
	t=$(hottest)
	if [ "$t" -eq 0 ]; then
		# No sensor answered. Full speed is the only safe response to not
		# knowing the temperature, and it is loud enough to be noticed.
		log "no temperature available; going to full speed"
		devmem $CPLD_PWM 8 $PWM_MAX 2>/dev/null
		cur=$PWM_MAX
		sleep $INTERVAL
		continue
	fi

	want=$(want_pwm "$t")
	if [ "$want" -lt "$cur" ]; then
		want=$((cur - STEP_DOWN))
		[ "$want" -lt "$PWM_MIN" ] && want=$PWM_MIN
	fi

	if [ "$want" -ne "$cur" ]; then
		devmem $CPLD_PWM 8 "$want" 2>/dev/null
		got=$(devmem $CPLD_PWM 8 2>/dev/null)
		got=$((got & PWM_MAX))
		if [ "$got" -ne "$want" ]; then
			log "asked for $want, register reads $got"
		fi
		cur=$got
		log "${t}C -> duty $cur/$PWM_MAX"
	fi
	sleep $INTERVAL
done
}

# ------------------------------------------------------------------ status
#
# Power supply decode, which is not what the register map suggests.
#
# PSU1's status is in 0x02 and PSU2's in 0x01 -- not two fields of one register
# -- and presence is ACTIVE LOW: bit 0 clear means a supply is fitted. Reading
# it the obvious way says no supply is present on a running switch, which is how
# EdgeNOS's own kernel driver gets it wrong; its Python layer overrides that
# with this map and notes it came from Cumulus.
#
# "present but not good" is the normal state for a second supply that is fitted
# with nothing plugged into it, so it is reported rather than treated as a
# fault.
psu() {
	reg=$1
	v=$(devmem $reg 8 2>/dev/null) || { echo "unreadable"; return; }
	v=$((v))
	p=fitted; g=no-power
	[ $((v & 1)) -eq 0 ] || p=absent
	[ $((v & 2)) -ne 0 ] && g=ok
	echo "$p, $g (reg $(printf 0x%02x $v))"
}

status() {
	v=$(devmem $CPLD_VERSION 8 2>/dev/null)
	printf 'cpld       version %d.%d\n' $(( (v >> 4) & 0xf )) $(( v & 0xf ))

	pwm=$(devmem $CPLD_PWM 8 2>/dev/null); pwm=$((pwm & PWM_MAX))
	printf 'fans       duty %d/%d (%d%%)  status %s\n' \
		"$pwm" "$PWM_MAX" $((pwm * 100 / PWM_MAX)) \
		"$(devmem $CPLD_FAN_STATUS 8 2>/dev/null)"

	printf 'psu1       %s\n' "$(psu $CPLD_PSU1)"
	printf 'psu2       %s\n' "$(psu $CPLD_PSU2)"
	printf 'leds       sys %s  locator %s\n' \
		"$(devmem $CPLD_LED_SYS 8 2>/dev/null)" \
		"$(devmem $CPLD_LED_LOC 8 2>/dev/null)"

	# Temperatures are not the CPLD's: the max6697 on I2C binds on its own and
	# gives seven sensors, with the limits the board ships.
	for f in /sys/class/hwmon/hwmon*/temp*_input; do
		[ -r "$f" ] || continue
		n=$(basename "$f" _input)
		d=$(dirname "$f")
		lbl=$n
		[ -r "$d/${n}_label" ] && lbl=$(cat "$d/${n}_label")
		crit=""
		[ -r "$d/${n}_crit" ] && crit=" (crit $(( $(cat "$d/${n}_crit") / 1000 ))C)"
		printf 'temp       %-8s %sC%s\n' "$lbl" "$(( $(cat "$f") / 1000 ))" "$crit"
	done
}

case "${1:-cool}" in
	cool)   cool ;;
	status) status ;;
	*)      echo "usage: $0 [cool|status]" >&2; exit 2 ;;
esac
