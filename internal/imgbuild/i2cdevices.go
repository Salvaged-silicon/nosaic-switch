/* SPDX-License-Identifier: Apache-2.0 */
package imgbuild

import (
	"fmt"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal/n3172tq"
)

// i2cDevicesScript renders a board's i2c declaration into a shell script that
// instantiates each part at boot.
//
// A script rather than a service exec line, for the reason bind-asic-irq.sh is
// one: a service's exec line is rendered into execline, where single quotes do
// not group, and this needs loops and command substitution.
//
// # TWO THINGS IT REFUSES TO HARDCODE
//
// The parent adapter is found by name. Adapter numbers come from probe order,
// and the mux adds more of them the moment its driver binds -- each channel
// becomes an adapter of its own -- so "bus 0" stops meaning what it meant.
//
// A mux channel is found through the mux's own sysfs symlink rather than by
// arithmetic on adapter numbers. i2c-mux-core creates channel-N under the mux
// device pointing at that channel's adapter, which is the interface the kernel
// actually promises; "parent + 1 + N" is a guess that happens to be true today.
func i2cDevicesScript(b *board.Board) string {
	if b.PlatformHAL.N3172TQ == nil || b.PlatformHAL.N3172TQ.I2C == nil ||
		(b.PlatformHAL.N3172TQ.I2C.Mux == nil && len(b.PlatformHAL.N3172TQ.I2C.Devices) == 0) {
		return ""
	}
	i2c := b.PlatformHAL.N3172TQ.I2C
	var s strings.Builder
	p := func(f string, a ...any) { fmt.Fprintf(&s, f+"\n", a...) }

	p(`#!/bin/sh`)
	p(`# Generated for %s. Tell the kernel about this board's i2c parts.`, b.ID)
	p(`#`)
	p(`# x86 has no device tree, so nothing else declares them: every driver is`)
	p(`# built and loaded and has nothing to bind to until something writes to`)
	p(`# new_device. Without this the board boots with acpitz and no sensors.`)
	p(`set -u`)
	p(``)
	p(`SYS=/sys/bus/i2c/devices`)
	p(`want=%q`, i2c.Adapter)
	p(``)
	p(`# The parent adapter, by name. See the note above about numbers.`)
	p(`parent=`)
	p(`for a in "$SYS"/i2c-*; do`)
	p(`	[ -r "$a/name" ] || continue`)
	p(`	case "$(cat "$a/name")" in`)
	p(`		*"$want"*) parent="${a##*/i2c-}"; break ;;`)
	p(`	esac`)
	p(`done`)
	p(`if [ -z "$parent" ]; then`)
	p(`	echo "i2c: no adapter matching \"$want\"; this board's sensors will be absent" >&2`)
	p(`	i2cdetect -l >&2 2>/dev/null || true`)
	p(`	exit 1`)
	p(`fi`)
	p(`echo "i2c: parent adapter i2c-$parent ($want)"`)
	p(``)
	p(`# Idempotent: a service that is restarted must not fail because the part`)
	p(`# it was asked to create is already there.`)
	p(`have() { [ -e "$SYS/$1-00$2" ]; }`)
	p(``)
	p(`# ⚠ new_device DOES NOT LOAD THE DRIVER.`)
	p(`#`)
	p(`# The i2c core matches a newly created device against drivers that are`)
	p(`# ALREADY registered. Turning a modalias into a modprobe is udev's job,`)
	p(`# and this image has no uevent handler doing it -- so with the driver`)
	p(`# built as a module, new_device succeeds, nothing claims the device, and`)
	p(`# the board comes up with the parts present and no hwmon entries. That`)
	p(`# is exactly what the first boot of this did.`)
	p(`#`)
	p(`# Failure is ignored: the driver may be built in, in which case there is`)
	p(`# no module of that name and nothing to do.`)
	p(`load() { modprobe "$1" 2>/dev/null || true; }`)
	p(``)
	p(`new() { # bus addr driver note`)
	p(`	if have "$1" "$2"; then`)
	p(`		echo "i2c: $3 at 0x$2 on i2c-$1 already present"`)
	p(`		return 0`)
	p(`	fi`)
	p(`	load "$3"`)
	p(`	if ! echo "$3 0x$2" > "$SYS/i2c-$1/new_device" 2>/dev/null; then`)
	p(`		echo "i2c: FAILED to add $3 at 0x$2 on i2c-$1 -- $4" >&2`)
	p(`		return 1`)
	p(`	fi`)
	p(`	# ⚠ CREATED IS NOT BOUND, and the difference is the whole point: an`)
	p(`	# unbound device has no hwmon entry, so the sensor silently does not`)
	p(`	# exist. Say so here rather than leaving it to be discovered as an`)
	p(`	# empty /sys/class/hwmon.`)
	p(`	if [ -e "$SYS/$1-00$2/driver" ]; then`)
	p(`		echo "i2c: $3 at 0x$2 on i2c-$1 -- $4"`)
	p(`	else`)
	p(`		echo "i2c: $3 at 0x$2 on i2c-$1 created but NO DRIVER BOUND -- $4" >&2`)
	p(`		return 1`)
	p(`	fi`)
	p(`}`)
	p(``)

	if i2c.Mux != nil {
		m := i2c.Mux
		p(`# The mux first: everything below is behind it.`)
		p(`new "$parent" %02x %q %q || exit 1`, m.Address, m.Driver, noteOr(m, "i2c mux"))
		p(``)
		p(`# A channel's adapter number, from the mux's own channel-N symlink.`)
		p(`MUX="$parent-00%02x"`, m.Address)
		p(`chan() {`)
		p(`	link=$(readlink -f "$SYS/$MUX/channel-$1" 2>/dev/null) || return 1`)
		p(`	[ -n "$link" ] || return 1`)
		p(`	base="${link##*/}"`)
		p(`	echo "${base#i2c-}"`)
		p(`}`)
		p(``)
	}

	p(`rc=0`)
	for _, d := range i2c.Devices {
		note := noteOr(&d, d.Driver)
		if d.Channel == nil {
			p(`new "$parent" %02x %q %q || rc=1`, d.Address, d.Driver, note)
			continue
		}
		p(`if bus=$(chan %d); then`, *d.Channel)
		p(`	new "$bus" %02x %q %q || rc=1`, d.Address, d.Driver, note)
		p(`else`)
		p(`	echo "i2c: mux channel %d did not appear; %s is absent" >&2`, *d.Channel, note)
		p(`	rc=1`)
		p(`fi`)
	}
	p(``)
	p(`# Non-zero is reported but does not take the service database down: a`)
	p(`# board missing one sensor should still boot and route.`)
	p(`exit $rc`)
	return s.String()
}

func noteOr(d *n3172tq.I2CDevice, fallback string) string {
	if d.Note != "" {
		return d.Note
	}
	return fallback
}
