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
slots by name. This board runs MBR, and its boot command loads from
`${usbdev}:5` — a *logical* partition, which only an MBR extended partition can
have. That is close to proof that this U-Boot was built for DOS partitions
alone.

The way out is not to abandon GPT for one board. U-Boot can read raw sectors
without parsing any partition table at all:

```
usb read 0x02000000 0x42000 0x7fc2      # load address, start LBA, sector count
```

That is the form the vendor OS used before it moved to `usbboot`, and it is
recorded as working on this exact board. So the installer writes the FIT to a
partition raw and points `nos_bootcmd` at that partition's LBA, which it knows
because it just wrote the table. The bootloader never needs to understand the
format.

**What this needs that does not exist yet.** NOSaic's boot partition is an ext2
filesystem holding the slot pointer, not a raw FIT. A U-Boot board needs
somewhere raw for the FIT, and both slots need one if A/B is to mean anything
here. That is a change to the disk layout for `boot: uboot`-underneath boards,
not a change to this board.

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


## What the netboot proved, 2026-09-01

It ran. Nothing was written to the switch -- `sda1 sda2 < sda5 sda6 > sda3`
enumerated during the boot, EdgeNOS's layout untouched -- and the box was
returned to EdgeNOS afterwards.

```
Bytes transferred = 53674684 (33302bc hex)
Using 'nosaic' configuration
Verifying Hash Integrity ... crc32+ OK        (kernel, ramdisk and fdt)
Booting using the fdt blob at 0x03000000
Uncompressing Kernel Image ... OK
Linux version 6.12.105 (nosaic@nosaic)
  (powerpc-nosaic-linux-gnu-gcc (crosstool-NG 1.28.0) 15.2.0)
Memory: 1997400K/2097152K available
Run /init as init process
NOSAIC-INITRAMFS booting from RAM, no partitions used
NOSAIC-INITRAMFS overlay assembled (persistent=no)
NOSAIC-BOOT userspace reached (s6)
NOSAIC-S6 compile rc=0
s6-rc-init: fatal: unable to supervise service directories in
            /run/s6-rc/servicedirs: Value too large for defined data type
```

So spike S1's instruction audit was right where QEMU could not be: a
soft-float generic-PowerPC toolchain on gcc-15.2.0 produces a kernel and a
userspace that this e500v2 executes. The whole boot chain works -- FIT, device
tree, big-endian kernel, squashfs, overlayfs, init handover.

**Two defects, and only real hardware would have found either.**

The first is in the list above: `EOVERFLOW` from `s6-rc-init`, and the cause is
that **reading a directory is enough**. Without `-D_FILE_OFFSET_BITS=64` glibc
gives a 32-bit program the narrow `readdir`, which refuses any entry whose
`d_ino` or `d_off` does not fit in 32 bits. `d_off` is not an offset -- it is an
opaque cookie the filesystem picks, and tmpfs picks large ones. So an ordinary
`readdir` of an ordinary tmpfs directory returns NULL with `errno` set, and
`s6rc_servicedir_manage` checks `errno` after its loop exactly as it should:

```c
errno = 0 ;
d = readdir(dir) ;
if (!d) break ;
...
if (errno) goto err ;
```

Fixed for every 32-bit architecture at once, because `off_t` and `ino_t` are
ABI: a library built one way and a program built the other disagree about
`struct stat` and nothing warns.

The second cost a boot to find. This U-Boot is from 2013 and knows crc32, md5
and sha1; the FIT was hashed with sha256. What it prints is:

```
Verifying Hash Integrity ... sha256 error!
Unsupported hash algorithm for 'hash' hash node in 'kernel' image node
Bad Data Hash
ERROR: can't get kernel image!
```

The last two lines arrive last and read like a corrupt image. The line that
says what is actually wrong is the one above them.

**Getting back is not automatic.** Magic sysrq is compiled in, the serial BREAK
trigger works, and `reboot(b)` is listed as permitted -- and sending it left the
board hard down, console silent to a CR, for several minutes. Only a power
cycle recovered it. Treat sysrq here as a way to *read* state, not to reset the
board, and have switched power on this box before booting anything at it.

## Trying it without installing

This is the way to run NOSaic on this switch today, and it is the direct
analogue of how the 7050SX2 was first booted — there, `boot http://...` from
the Aboot prompt; here, TFTP from the U-Boot prompt. **Nothing is written.** No
partition, no filesystem, no `saveenv`. Power-cycling the switch returns it to
exactly the OS it was running before.

The build emits a second artifact for this:

```
make image BOARD=edgecore-as5610-52x ARGS=--ram-boot
  NOSaic-<version>-edgecore-as5610-52x.itb    ~51 MiB
```

A FIT carrying the kernel, the device tree, and an initramfs with the whole
root filesystem inside it. Built for any board that declares U-Boot addresses,
whatever its installer, precisely so that an ONIE board can be tried before it
is committed to.

Serve it from the build host and, at the `LOADER=>` prompt:

```
setenv autoload no
dhcp                                  # or set ipaddr/netmask/gatewayip by hand
setenv serverip <build-host>
setenv bootargs 'console=ttyS0,115200 cma=32M'
tftpboot 0x08000000 NOSaic-<version>-edgecore-as5610-52x.itb
bootm 0x08000000#nosaic
```

Every line of that is load-bearing:

- **`bootargs` must be set.** Without it the kernel falls back to its built-in
  command line, and the failure is a board that loads the image and then prints
  nothing at all. No panic, no console, no clue.
- **`0x08000000`, not the vendor's `0x02000000`.** The vendor stages its own
  5 MB image at 32 MB. Ours carries the root filesystem, so the initramfs
  unpacks from 0x03100000 to roughly 0x059b0000 — staging at 0x02000000 puts
  the image on top of where its own contents are going, and the copy overwrites
  its source partway through.
- **`#nosaic`** names the configuration inside the FIT.
- **`setenv`, never `saveenv`.** The addresses live in RAM and are gone on the
  next reset. A `saveenv` during bring-up is how a board ends up unable to boot
  anything.

Do **not** set `fdt_high` or `initrd_high`. EdgeNOS's notes record setting them
to `0xffffffff` as breaking both its own boot and ONIE's; U-Boot's default
relocation is correct here.

## The FIT this board wants

Read off the image the switch boots today, not chosen:

| | |
|---|---|
| kernel | gzip, `load` and `entry` both `0x0` — `CONFIG_RELOCATABLE=y` does the rest |
| device tree | uncompressed, `load = 0x03000000` |
| initramfs | **`compression = "none"`**, `load = 0x03100000` |

The initramfs is a `.cpio.gz`, and declaring it `none` is not an oversight: the
kernel unpacks it. Telling U-Boot it is gzip makes U-Boot decompress it first,
into a buffer sized for something else.

Two more, both of which cost someone a day already:

- **A ramdisk node is required.** U-Boot 2013.01 here fails silently on a FIT
  that has none.
- **Never nest a uImage inside a FIT.** A PowerPC `make uImage` is a legacy
  U-Boot header wrapped around a gzipped self-extracting zImage; put that whole
  thing in as `type = "kernel"` and `bootm` executes the header bytes. The
  payload is what belongs there — the first 64 bytes off the front. NOSaic's
  U-Boot backend does this unwrapping itself, so the kernel recipe stages one
  artifact and the boot backend takes what it needs.

## Getting back

Three routes, in increasing order of how broken things are.

**From a running OS**, no console needed. `onie_boot_reason` is the whole
decision: U-Boot's `check_boot_reason` sends the box to ONIE if it is set to
anything, and ONIE deletes it once it has done its job.

```
fw_setenv onie_boot_reason install && reboot
```

`fw_setenv` prompts for confirmation and will sit there waiting in a script, so
pipe `y` into it. This needs `/proc/mtd`, which needs the MTD options in
`recipes/linux/config/powerpc.fragment` — without them the NOR never probes,
`/proc/mtd` is empty, and `fw_setenv` has nothing to write.

**From the `LOADER=>` prompt**, when the OS will not start:

```
setenv onie_boot_reason install
saveenv
boot
```

**When the environment itself is wrong** and neither the NOS nor ONIE will
start:

```
env default -a
saveenv
reset
```

That restores U-Boot's compiled-in defaults and lands in ONIE.

Underneath all three is the hardware guarantee: **ONIE and U-Boot are in NOR
flash, and the device tree marks `onie`, `uboot` and `board_eeprom`
read-only** — the kernel's MTD layer refuses writes to them. Only
`u-boot-env` is writable. Nothing NOSaic can do to `/dev/sda`, and nothing
short of a deliberate `flash_erase` of a read-only MTD, removes the way back.

**And the vendor OS is a way back too.** ONIE installers for the Cumulus Linux
releases this board shipped with are kept outside this repository; from ONIE,
`onie-nos-install <url>` returns the switch to one of them.

## What ONIE's shell does not have

Its BusyBox is from 2017 and the installer runs inside it. Verified absent:
`parted`, `partprobe`, `ssh-keygen`, `curl`, `mkfs.ext4`. Present: `fdisk`,
`blkid`, `mke2fs` (but not its `-q` flag), `dd`, `tar`, `wget`, `fw_printenv`,
`fw_setenv`.

Two traps worth stating plainly, because both produce a *successful* install
that does not boot:

- **`PATH` is `/usr/bin:/bin`.** `fdisk`, `mke2fs`, `fw_setenv` and `reboot`
  all live outside it. NOSaic's installer exports a full `PATH` first.
- **ONIE reports failure on a successful install, and then undoes it.** Its
  `exec_installer` calls `reboot` without a path; on this box that fails, the
  error handler runs, and the step that resets the NOS boot command runs with
  it. So the installer reboots the switch itself, with a full path, before
  handing control back.
