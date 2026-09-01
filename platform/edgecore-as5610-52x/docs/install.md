# Installing NOSaic on the AS5610-52X

Nothing has been installed on this board. This page records what the install
route is, from the flash layout on a running unit.

## What the board boots

U-Boot, then ONIE, from NOR flash. There is no eMMC and no partition table:

```
mtd0   0x00360000   onie            3.4 MB
mtd1   0x00010000   u-boot-env
mtd2   0x00010000   board_eeprom
mtd3   0x00080000   uboot           512 KB
```

`onie_platform` is `powerpc-accton_as5610_52x-r0`. An ONIE installer must
declare a matching platform or ONIE will refuse it, and that string is the one
to match — confirm with `onie-sysinfo -p` on the box rather than trusting this
page.

## Why that changes things

The 7050SX2's install work does not transfer. There, Aboot resolves `flash:`
against a FAT filesystem on eMMC, and NOSaic's persistent site configuration
lives in a directory on that partition. Here there is neither: the flash is
MTD, the installer is ONIE, and where a switch's own configuration lives has to
be answered again for this board.

`board_eeprom` being its own MTD partition is the other difference. The board's
identity and MAC addresses live there; the 7050SX2 keeps the same information
on an i2c SEEPROM read through its board controller. A board that cannot read
its own MAC comes up with a random one that changes every boot, which is worth
solving before the first install rather than after.

## The route

`boot: onie-sfx` — NOSaic emits a self-extracting installer that ONIE runs.
The backend exists (`internal/boot/onie.go`) and has never produced an image
that a switch has accepted.

**Recovery.** ONIE's own rescue mode is the way back, and it lives in `mtd0`
independently of anything NOSaic writes. That is a better position than the
7050SX2 started from, where the recovery path had to be established first.
Do not write `mtd0` or `mtd3`.
