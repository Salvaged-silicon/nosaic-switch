# Installing NOSaic on the AS5610-52X

**This works.** Installed with `onie-nos-install` on 2026-09-03 and verified on
the hardware: booted from disk with no netboot involved, overlay on slot A over
`/dev/sda4`, every service up, the front panel lit and four OSPF adjacencies
full. Before that, NOSaic only ever ran here because somebody typed a
`tftpboot` at the U-Boot prompt, and a plain reboot returned the switch to the
vendor's NOS.

Everything below is what the mechanism does and why. It is written out because
three of the four things that went wrong presented as something other than what
they were, and the next person deserves better than rediscovering them.

---

## 1. What actually boots this board

Four pieces of firmware and software hand off in order. None of it is BIOS or
GRUB, and none of it reads a GPT.

```
U-Boot 2013.01  (NOR flash, read-only)
  └─ bootcmd = run check_boot_reason; run nos_bootcmd; run onie_bootcmd
       ├─ check_boot_reason : if onie_boot_reason is set, go to ONIE
       ├─ nos_bootcmd       : boot the installed NOS   <-- what we install into
       └─ onie_bootcmd      : fall back to ONIE
```

The three are **sequential, not alternatives**. If `nos_bootcmd` fails to boot
anything, control simply arrives at `onie_bootcmd` and the switch comes up in
ONIE. That is the safety net, and it is also the thing that disguises a failed
boot as an ONIE problem.

`nos_bootcmd` after a NOSaic install:

```sh
usb start; usbiddev;
setenv bootargs console=ttyS0,115200 cma=32M;
usbboot 0x10000000 ${usbdev}:1 && bootm 0x10000000#nosaic
```

Read it left to right: bring up USB, work out which USB device is the disk,
set the kernel command line, read **partition 1 as raw blocks** into RAM at
`0x10000000`, and boot the FIT found there using the configuration named
`nosaic`.

Three properties of that line matter more than they look.

**`usbboot` is a block read, not a file read.** It does not mount anything. The
partition it names has no filesystem — it is a FIT image written to raw
sectors. This board's own disk is a USB DOM, which is why the command says
`usb` at all.

**`${usbdev}` is expanded by U-Boot at boot, not by anything else.** It is set
by `usbiddev` a moment earlier. Anything that writes this string has to keep
those eight characters literal; see §5.

**`#nosaic` selects a configuration inside the FIT.** The vendor's own command
ends `#accton_as5610_52x`, because that is what the vendor's FIT contains. Ours
says `nosaic` because ours does. The name in the boot command and the name in
the image must agree, and if they do not, `bootm` fails and the switch lands in
ONIE with nothing on the console to say why.

---

## 2. The disk layout, and why it is not the usual one

Every other NOSaic board gets a GPT with four named partitions. This one gets a
**DOS/MBR table**, because **this U-Boot has no GPT support at all**.

That is measured, not inferred. `/dev/mtd3` is the U-Boot image and is readable
from a running OS; dumped and searched, it contains no `EFI`, `GUID` or GPT
strings anywhere — only `## Unknown partition table`. It does contain
`ext2load`, `fatload`, `usbboot` and `usbiddev`. A GPT install here produces a
switch that cannot find anything on its own disk.

```
partition_table: dos
fit_mib: 24
slot_mib: 384
data_mib: 512
```

| part | size | contents | how it is read |
|---|---|---|---|
| `sda1` | 24 MiB | the FIT: kernel + initramfs + DTB | raw, by U-Boot `usbboot` |
| `sda2` | 384 MiB | slot A — squashfs root filesystem | raw, mounted by the initramfs |
| `sda3` | 384 MiB | slot B — empty until the first upgrade | raw |
| `sda4` | 519 MiB | data: config, secrets, per-slot overlays | ext4, label `nosaic-data` |

**There is no separate boot partition, and that is a consequence of MBR.** A DOS
table has four primaries; the FIT takes one, so the slot pointer that records
A/B state moves onto the data partition (`/mnt/data/boot`). It is still
persistent, so trial boots and rollback still mean something. The initramfs
looks for a `nosaic-boot` filesystem first and falls back to the data partition
when there is not one.

**Slot A and B are found by device order, not by name.** MBR partitions have no
names, so `findfs LABEL=nosaic-slot-a` finds nothing and the initramfs falls
back to `/dev/sda2` and `/dev/sda3`. The data partition is still found by label,
because a *filesystem* label survives regardless of the partition table.

**One FIT serves both slots.** An upgrade replaces the kernel for A and B
together, so a rollback returns the previous *root filesystem* under the current
kernel. That is a real limitation. Two FIT partitions would need an extended
partition or firmware that can choose between them.

---

## 3. What the installer is

A `.bin` that ONIE executes directly: a POSIX shell script with a tar archive
appended after the line `__NOSAIC_PAYLOAD__`. The script locates the payload by
finding that marker in **itself**, so editing a comment above cannot corrupt the
offset.

```
NOSaic-<version>-edgecore-as5610-52x.bin   ~70 MiB
├── #!/bin/sh … the installer                (a few KB)
└── __NOSAIC_PAYLOAD__
    └── tar, uncompressed
        ├── disk.img.gz      the whole disk: table + slot A + data
        └── …itb             the FIT
```

**The tar is uncompressed and the disk image inside it is gzipped.** That
combination is deliberate. The tar stays plain so the installer can stream one
member out of it with `tar -xO` and nothing else; the disk image is compressed
because it is mostly zeros and because of §4.

### What it does, in order

1. **Set a usable `PATH`.** ONIE gives an installer `/usr/bin:/bin` and nothing
   else. `fdisk`, `mke2fs`, `fw_setenv`, `blockdev` and `reboot` all live in
   `/sbin` or `/usr/sbin`, and every one of those failures reads as "not found"
   for a tool that is plainly installed.
2. **Choose the disk.** `$onie_boot_dev` if ONIE said, otherwise `/dev/sda`,
   and it says out loud when it is guessing.
3. **Write the disk image**, table and all, streamed through `gunzip` straight
   into `dd`. Installing means replacing the layout, not fitting inside ONIE's.
4. **Write the FIT at an absolute byte offset** — not to `/dev/sda1`. See §5.
5. **Verify the FIT**, by reading back four bytes from that offset and checking
   for the FDT magic `d0 0d fe ed`. If it is not there the installer stops
   rather than leaving a switch that will fall through to ONIE saying nothing.
6. **Set `nos_bootcmd`** with `echo y | fw_setenv`, then read it back.
7. **Clear `onie_boot_reason`**, or the switch installs perfectly and comes
   straight back to the installer.
8. **Reboot** with an explicit `/sbin/reboot`.

---

## 4. What ONIE does around it, and what that costs

### ONIE stages the installer in RAM

`onie-nos-install` downloads the entire `.bin` into a tmpfs before executing it.
On this board `/tmp` is **1012 MiB**, out of 2 GB of RAM.

An uncompressed image was 1322 MiB and failed two thirds of the way through
with:

```
wget: short write
```

which reads exactly like a network fault and is not one — it is a full
filesystem. **Installer size is bounded by memory, not by flash.** Compressing
the disk image took the same install to 70 MiB.

### ONIE boots into install mode by default

Its own banner says it: *"ONIE started in NOS install mode. Install mode
persists until a NOS installer runs successfully."* While in that mode, on every
boot, `/lib/onie/boot-mode-arch` runs `install_remain_sticky_arch()`:

```sh
# Delete the one time onie_boot_reason variable.  Also set
# nos_bootcmd to a no-op, which will keep us in this state
onie_boot_reason
nos_bootcmd true
```

So **ONIE deliberately sets `nos_bootcmd=true`** to stay in control.

This is worth dwelling on, because it produces a badly misleading symptom. If
the NOS fails to boot, U-Boot falls through to ONIE, ONIE comes up in install
mode, and it sets `nos_bootcmd=true`. The next time anyone looks, the boot
command has been clobbered — and it is natural to conclude that ONIE is
overwriting the installer's work. It is not. **A clobbered `nos_bootcmd` is a
*symptom* of a NOS that did not boot, not the cause.** Fix the boot, and the
variable stays.

### `fw_setenv` prompts

ONIE's `fw_setenv` asks `Proceed with update [N/y]?` and waits. Non-interactively
it consumes stdin and silently writes nothing, so the install "succeeds" and the
switch boots ONIE again. Feed it `echo y |`.

### Reaching ONIE, and its two modes

From a running OS, with no serial console:

```sh
fw_setenv onie_boot_reason install   # or rescue | uninstall | update
reboot
```

- **`install`** brings up networking and the discover daemon. It gets an address
  from DHCP, and **it will not be the NOS's address** — find it by MAC. Both
  telnet and SSH are open. SSH needs legacy crypto:
  `-o HostKeyAlgorithms=+ssh-rsa -o KexAlgorithms=+diffie-hellman-group1-sha1
   -o Ciphers=+aes128-cbc,3des-cbc`
- **`rescue`** gives a shell but did not bring up networking here. It is
  self-clearing, so a power cycle returns to the installed NOS — safe to enter,
  useless remotely.

`onie-sysinfo -p` returns `powerpc-accton_as5610_52x-r0` — **underscores, not
hyphens**. Any installer that checks the platform string must match that
exactly.

---

## 5. The two bugs worth remembering

### `${usbdev}` must survive being written

The boot command is embedded in the installer, which is a shell script. Written
inside double quotes, **the installer's own shell expands `${usbdev}`** — it is
unset under ONIE — and writes this into the firmware:

```
usbboot 0x10000000 :1
```

An install that reports complete success and boots nothing. Both generated
values are now assigned once as **single-quoted** shell variables, and the build
refuses a value containing a single quote rather than emitting something broken.

### The kernel does not know the partitions changed

`dd` a new partition table onto a disk and the kernel carries on describing the
old one. ONIE's busybox has no `partprobe`. So `/dev/sda1` still exists, and
still points at the **previous** owner's first partition — on this board
EdgeNOS's, starting at sector 8192, where ours starts at 2048.

`dd of=/dev/sda1` therefore wrote the FIT 3 MiB into the new partition instead
of at its start. Every command succeeded; the firmware read zeros. The installer
now writes at an absolute offset carried from the table the image was built
with, and reads it back before continuing. `blockdev --rereadpt` is called too —
it does exist here — but correctness no longer depends on it.

---

## 6. Installing

```sh
# 1. Build. The image and the installer come out together.
make image BOARD=edgecore-as5610-52x

# 2. Serve it where the switch can fetch it.
python3 -m http.server 8080 --bind <build-host>

# 3. From the running NOS, ask for ONIE.
ssh root@<switch> 'echo y | fw_setenv onie_boot_reason install; /sbin/reboot'

# 4. Find ONIE. It takes a DIFFERENT DHCP address; look it up by MAC from
#    another host on the same subnet.
ssh root@<other-switch> 'for i in $(seq 1 254); do ping -c1 -W1 10.1.1.$i >/dev/null 2>&1 & done; wait
                         ip neigh | grep -i <the-switch-mac>'

# 5. Install.
ssh $LEGACY_CRYPTO root@<onie-addr> \
  'PATH=/usr/sbin:/sbin:/usr/bin:/bin; export PATH
   onie-nos-install http://<build-host>:8080/NOSaic-…-edgecore-as5610-52x.bin'
```

The switch reboots itself into NOSaic. `make image` without `--ram-boot` is the
installable one; with it, the root filesystem is carried inside the initramfs
for a diskless TFTP boot and the artifact is far too big to install.

---

## 7. Getting back

**The board cannot be bricked from software.** Of the four NOR flash regions,
only `mtd1` (`u-boot-env`) is writable — `onie`, `uboot` and `board_eeprom` are
marked read-only in the device tree, so neither the OS nor ONIE can damage the
bootloader or the recovery image.

| situation | way back |
|---|---|
| NOSaic boots but is broken | `fw_setenv onie_boot_reason install; reboot` |
| NOSaic does not boot at all | U-Boot falls through to ONIE by itself |
| the U-Boot env is wrong | from ONIE: `fw_setenv nos_bootcmd …` |
| the env is unusable | at the U-Boot prompt: `env default -a; saveenv; reset` — needs the serial console |

⚠ **Do not set `fdt_high` or `initrd_high`.** `newnos/BOOT.md` records that
setting either to `0xffffffff` breaks this NOS *and* ONIE. The installer writes
`nos_bootcmd`, clears `onie_boot_reason`, and touches nothing else.

Restoring the vendor NOS needs a backup taken beforehand — there is no installer
image for it on the build host. See `~/backups/as5610-edgenos-20260903/`.

---

## 8. What this does not do yet

- **A/B is one-sided for the kernel.** Rollback restores the root filesystem,
  not the FIT. §2.
- **The installer does not check the platform string.** It installs onto
  whatever it is run on. `onie-sysinfo -p` is the value to compare.
- **No trial boot on the *first* install.** A fresh install goes straight to
  slot A and commits, because there is no known-good slot to fall back to yet.
  Every later upgrade is a trial: on 2026-09-04 an image installed into the
  inactive slot booted as a trial and **committed itself** here, judged by the
  switch rather than by anyone watching. `nosaic upgrade status` shows the
  pointer and `nosaic upgrade confirm` is the explicit form.
- **`onie_boot_reason=rescue` is not useful remotely** — no networking. Use
  `install`.

## See also

- `newnos/installer/ONIE_ISSUES.md` — thirteen ONIE compatibility findings on
  this exact board. Read it before changing the installer.
- `newnos/BOOT.md` — the FIT format, the U-Boot variables, and what must never
  be set.
- `newnos/docs/FLASH_MTD_AND_ONIE_RECOVERY.md` — the flash map and why only the
  environment is writable.
