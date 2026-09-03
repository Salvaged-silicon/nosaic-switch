#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Fan control for the AS5610-52X.
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

CPLD_PWM=0xea00000d

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
