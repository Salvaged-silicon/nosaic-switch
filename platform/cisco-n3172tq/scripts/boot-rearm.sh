#!/bin/sh
# Re-arm the firmware's one-shot "boot the EFI shell" request.
#
# BdsDxe consumes the CMOS boot record when it acts on it, so without this
# every boot after the first falls to the vendor `loader>` prompt. Record
# layout (CMOS index port 0x72, data port 0x73, indexes 0xC8..0xDB):
#
#   b0  = ~(sum of b1..b19) & 0xff, so 0xe7 for the record below
#   b2  = 0x0a, b3 = 0x0b   boot "EFI Internal Shell" once
#   b9  = 3                 loader bootmode g2p: the loader must EXIT on a
#                           failed autoboot, or the firmware is never reached
#   all other bytes zero
#
# Always exits 0: a failed re-arm must not hold up the datapath.

wr() {
	printf "\\$(printf %03o "$1")" | dd of=/dev/port bs=1 seek=114 conv=notrunc 2>/dev/null
	printf "\\$(printf %03o "$2")" | dd of=/dev/port bs=1 seek=115 conv=notrunc 2>/dev/null
}
rd() {
	printf "\\$(printf %03o "$1")" | dd of=/dev/port bs=1 seek=114 conv=notrunc 2>/dev/null
	dd if=/dev/port bs=1 skip=115 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n'
}

i=201
while [ "$i" -le 219 ]; do wr "$i" 0; i=$((i + 1)); done
wr 202 10
wr 203 11
wr 209 3
wr 200 231

rec=
i=200
while [ "$i" -le 219 ]; do rec="$rec $(rd "$i")"; i=$((i + 1)); done
echo "boot-rearm: CMOS record:$rec"
[ "$rec" = " e7 00 0a 0b 00 00 00 00 00 03 00 00 00 00 00 00 00 00 00 00" ] \
	|| echo "boot-rearm: WARNING record does not read back as written"
exit 0
