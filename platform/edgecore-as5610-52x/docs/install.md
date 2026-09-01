# Installing NOSaic on the AS5610-52X

Nothing has been installed on this board. This page records what the install
route is, from the flash layout on a running unit.

## What the board boots

U-Boot, then ONIE. `onie_platform` is `powerpc-accton_as5610_52x-r0`; an ONIE
installer must declare a matching platform or ONIE refuses it. Confirm with
`onie-sysinfo -p` on the box rather than trusting this page.

There are two storage devices and the distinction matters.

**NOR flash, 8 MB, as MTD.** Firmware only:

```
mtd0   0x00360000   onie            3.4 MB
mtd1   0x00010000   u-boot-env
mtd2   0x00010000   board_eeprom
mtd3   0x00080000   uboot           512 KB
```

**NAND, ~3.8 GB, as a block device** — `/dev/sda`, with an ordinary partition
table. This is where an operating system goes:

```
sda1    127 MB
sda2          extended
sda3   3386 MB
sda5   15.9 MB
sda6    289 MB
```

## Why this is easier than the 7050SX2

The Arista board boots a single `.swi` that Aboot loads from a FAT filesystem
on eMMC, and NOSaic's A/B slot machinery has never run on it: there is one
image, loaded directly, and the site configuration had to be given a home on
the boot partition because there was nowhere else.

This board has a real block device with a real partition table, and the vendor
OS already uses the shape NOSaic was designed around — a read-only squashfs
under an overlay:

```
overlay on /  lowerdir=/lower  upperdir=/rw/config1/upper  workdir=/rw/config1/work
```

`config1` rather than `config` suggests slots, and the switch database entry
for this board records `persistence: squashfs-overlay` and dual-slot A/B
upgrade. So the A/B mechanism that is CI-tested on the virtual platform and
unexercised on the Arista has a plausible home here, and site configuration has
somewhere obvious to live rather than needing a special case.

`board_eeprom` on the NOR flash is where the board's identity and MAC addresses
live. A board that cannot read its own MAC comes up with a random one that
changes every boot, so this is worth solving before the first install rather
than after.

## The route

## The route

`boot: onie-sfx` — NOSaic emits a self-extracting installer that ONIE runs.
An image now builds:

```
vmlinuz             9.6 MiB   u-boot legacy uImage, Linux/PowerPC
initramfs.cpio.gz   1.2 MiB
rootfs.sqsh        40.5 MiB
NOSaic-...bin       488 MiB   the installer: a script with a disk image appended
```

The kernel is a genuine PowerPC uImage, which is what U-Boot on this class of
board can start. **The installer is not yet safe to run**, for two reasons
found by reading what it produces rather than by trying it.

### It does not tell U-Boot anything

The generated script writes the whole disk image to the device ONIE booted
from, syncs, and says "reboot to start it". Nothing sets `nos_bootcmd` in
`u-boot-env`, which is how an ONIE platform tells U-Boot where its NOS lives.
On an x86 ONIE box firmware can find a bootloader in the image's own boot
partition; a PowerPC U-Boot board has to be told.

`internal/boot/onie.go` contains no mention of U-Boot at all, so this is a gap
in the backend rather than a mistake in the board.

### The layout is GPT and this board's is MBR

NOSaic's disk image is GPT with named partitions — the initramfs finds its
slots by name. This board currently runs an MBR layout with an extended
partition. Whether the vendor U-Boot can read GPT is unknown and needs checking
before anything is written.

### Why this is still recoverable

ONIE and U-Boot live on the NOR flash, which nothing in this path touches. A
bad disk image leaves both intact, and ONIE rescue is the way back. That is a
better starting position than the 7050SX2 had.

**Recovery.** ONIE's own rescue mode is the way back, and it lives on the NOR
flash independently of anything NOSaic writes to `/dev/sda`. That is a better
position than the 7050SX2 started from, where the recovery path had to be
established before anything else could be risked.

**Do not write `mtd0` or `mtd3`** — ONIE and U-Boot. Everything NOSaic installs
belongs on the block device.
