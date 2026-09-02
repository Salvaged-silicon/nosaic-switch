# Edgecore AS5610-52X — what is left

Everything. Nothing NOSaic has built runs on this board, and this page is the
order to attack it in rather than a list of defects.

Status of the board is in [the README](../README.md); what is known about the
hardware is in [hardware.md](hardware.md), and almost all of it was read off a
unit running EdgeNOS rather than produced here.

## Required — nothing works without these

### 32-bit large-file support — fixed, not yet re-verified on hardware

The board's first boot reached `/sbin/init` and stopped in `s6-rc-init` with
`EOVERFLOW`. Every 32-bit package now builds with `-D_FILE_OFFSET_BITS=64`;
what remains is to rebuild the PowerPC packages and repeat the netboot.

Worth stating because it will come up again on the next 32-bit board: this is
an ABI-wide switch, not a per-package workaround. `off_t` and `ino_t` change
size, so a library built one way and a program built the other disagree about
`struct stat` silently.

EdgeNOS never met this, and the reason is instructive rather than incidental:
its userland is a **Buildroot 2023.02.9** base, and Buildroot turns large-file
support on for every 32-bit target as a matter of course. Inheriting a
distribution's userland inherits its defaults. Building one from source means
owning them, and this is the first of those defaults NOSaic has had to
discover for itself.

**Still open: `_TIME_BITS=64`.** 32-bit `time_t` overflows in January 2038, and
a project whose premise is keeping abandoned hardware running is exactly the
project that will still have these boards then. glibc 2.42 supports it and it
depends on `_FILE_OFFSET_BITS=64`, which is now set — but it is a second
ABI-wide change and it wants doing deliberately, with every 32-bit package
rebuilt together, rather than folded into a boot fix.

### ~~The PowerPC toolchain~~ — spike S1 passed

**Done, and it was the biggest unknown here.** `make toolchain ARCH=powerpc`
builds in 38 minutes and `make toolchain-test ARCH=powerpc` passes:

```
hello: ELF 32-bit MSB executable, PowerPC, statically linked
ran:   nosaic bits=32 endian=big ptr=4 long=4
instruction audit: 0 forbidden
abi floor: Linux 3.2.0 (ceiling 4.0.0)
```

The question S1 existed to answer was whether a modern compiler can still
produce e500v2-safe binaries at all, given that GCC dropped the SPE ABI target
around GCC 8/9. It can: soft-float generic PowerPC on gcc-15.2.0 and
glibc-2.42, with the instruction audit confirming nothing in the output uses an
opcode this CPU cannot execute. This class of hardware does **not** need to be
frozen on a 2021 toolchain, and M8's scope is unchanged.

The audit is the part to keep. A hard-float build disassembles cleanly, runs
under QEMU's permissive generic-PowerPC model, and dies only on the board — so
`arch/powerpc/arch.yml` fails any build containing one of those instruction
classes. Do not weaken it to make something link.

### ~~The kernel cannot build a device tree~~ — done

x86_64 boards describe themselves through ACPI and need no device tree, so
nothing in the kernel recipe had ever compiled one. This board cannot boot
without one.

Resolved the way the plan said — DTS as board data. A board names one in
`board.yml`:

```yaml
device_tree: dts/as5610-52x.dts
```

and the image builder compiles it and hands it to the boot backend, which
carries it inside the FIT. Compiled at image time rather than committed as a
`.dtb`, because a device tree is the one board file where a wrong value hangs
the board before the console opens — it needs to be readable in a diff. `dtc`
was already a build dependency: `mkimage` shells out to it.

The source is EdgeNOS's, itself derived from ONL's. Unlike the 7050SX2's port
map and polarity tables, which are vendor-derived and stay out of the public
tree, this is open-source-derived board data and ships in it.

Two smaller things were fixed while establishing this, and are worth knowing
because both would have produced a kernel that builds and does not boot:

- `arch/powerpc/arch.yml` named `corenet32_smp_defconfig`. The P2020 is an
  mpc85xx part -- `CONFIG_PPC_P2020` is in `platforms/85xx/Kconfig` with its
  own `p2020.c` -- and CoreNet starts with the e500mc parts. It is
  `mpc85xx_smp_defconfig`, `_smp` because the P2020 is dual core.
- `kernel_image` was `vmlinux`. `CONFIG_PPC_P2020` selects `DEFAULT_UIMAGE`,
  and U-Boot on this class of board loads a uImage; a bare vmlinux is not
  something it can start.

### The ONIE backend does not handle U-Boot platforms

An installer builds — script plus a 488 MB disk image, with a genuine
PowerPC uImage kernel inside it — and it would still not produce a bootable
switch. Two of the four things wrong with it are fixed; two are not.

**Fixed.** The installer exports a full `PATH` before it does anything (ONIE
gives it `/usr/bin:/bin`, and `fdisk`, `mke2fs`, `fw_setenv` and `reboot` all
live outside that), and it reboots the switch itself with `/sbin/reboot`.
The second matters more than it looks: ONIE's own `exec_installer` calls
`reboot` without a path, that fails here, and its error handler then re-runs
the step that resets the NOS boot command. A successful install undoes itself.

**Still open — the FIT has nowhere to live.** `internal/boot/onie.go` writes
the disk and stops; nothing sets `nos_bootcmd`, which is how an ONIE platform
tells U-Boot where its NOS is. The reason that is not a two-line fix is the
layout: NOSaic's boot partition is an ext2 filesystem holding a slot pointer,
and a U-Boot board needs the FIT somewhere raw — one per slot, if A/B is to
mean anything here.

**Still open — GPT against a DOS-only U-Boot.** NOSaic's image is GPT with
named partitions. This board's boot command reads `${usbdev}:5`, a *logical*
partition, which only MBR has; that is near proof its U-Boot has no GPT
support. The answer is not to give this board a different table but to stop
needing one: `usb read <addr> <lba> <count>` takes raw sectors and parses
nothing, and it is recorded as working on this exact board. The installer knows
the LBA because it just wrote the table.

Both remaining items are backend work rather than board work, which is the
right shape: the board declares `boot: onie-sfx` and should not have to know.

**Meanwhile there is a way to run on this board that needs none of it.** The
build now emits a netboot FIT for any board that declares U-Boot addresses,
whatever its installer, and TFTP into RAM writes nothing at all. See
[install.md](install.md). That is the analogue of how the 7050SX2 was first
booted, and it is the right order: prove the image runs before designing the
partition table it will live in.

### Overlay upper has no xattr support in a RAM boot

The first boot logged:

```
overlayfs: failed to set xattr on upper
overlayfs: ...falling back to redirect_dir=nofollow.
overlayfs: ...falling back to uuid=null.
```

`CONFIG_TMPFS_XATTR` is not set, and in a RAM boot the overlay's upper layer is
a tmpfs. Overlayfs fell back and the boot continued, so this is not what stopped
it -- but the fallback is not free: opaque directory markers are an xattr, and
without them removing a directory that exists in the lower layer and recreating
it does not behave the way it should. On a disk boot the upper is ext4 and the
question does not arise, which is why no board has met it before.

Deliberately not changed while the large-file fix is being verified -- one
variable at a time, and it costs a full kernel rebuild.

Related, from EdgeNOS's build notes and not yet a problem here: `mksquashfs` on
a container overlay filesystem can fail trying to write `security.selinux` and
exit non-zero. Their fix was `-no-xattrs` on both mksquashfs and unsquashfs.
NOSaic's builds have not hit it.

### The board's own hardware is undescribed

There is no platform HAL for it. From EdgeNOS's manifest it needs at least a
CPLD driver, a **DS100DF410 40G retimer** with an init step, sensors and SFP
through something ONLP-shaped, and **LED microcontroller firmware**
(`led0.hex`, `led1.hex`).

The retimer is the one that will present as a mystery: an unprogrammed retimer
is a 40G link that does not come up, and on the sibling 7050TX-64 leaving the
equivalent part in reset cost *every* port on the box rather than only the 40G
ones.

The LEDs are the opposite arrangement to the 7050SX2, where the chip's LED
processors are off and the board controller drives the panel directly. Here
there is firmware to load, and NOSaic has never done that on any board.

### Where site configuration lives has to be answered again

The 7050SX2 keeps a switch's own settings in a directory on its FAT flash
partition. This board has no eMMC and no partition table — NOR flash as MTD,
with ONIE in `mtd0` and `board_eeprom` as its own partition. The mechanism
does not transfer.

`board_eeprom` is also where this board's identity and MACs live. A board that
cannot read its own MAC comes up with a random one that changes every boot,
which is worth solving before the first install.

## Known blocker inherited from EdgeNOS

### The field processor does not evaluate live traffic

EdgeNOS reached the point where an ACL is installed in the TCAM, reads back
correctly, and **never matches a packet**. 2000 injected IPv4 packets to a
drop-ACL'd destination flooded through the chip with the FP statistic at zero.

Exhaustively ruled out there: `IFP_BYPASS_ENABLE=0`, the slice map, the port
field select (`fpf2=0` is the correct registered selcode for a DstIp-only
group), entry validity, `init misc`, and the arming registers edged uses.

It matters to NOSaic because the tap bridge's punt path on the 7050SX2 is built
on field-processor rules. If the FP cannot be armed on this chip, the punt path
needs a different mechanism here — so this should be established early rather
than discovered after the datapath is otherwise working.

The recommended next step, from that work: install a drop ACL under Cumulus on
this exact silicon, confirm it drops, dump the ingress-pipeline enable and
config registers, and diff against the SDK's state. The differing register is
the missing arming.

## Not blockers, but decide early

- **Interrupts.** EdgeNOS runs this chip polled. On the 7050SX2 that cost a
  full core and held the control plane to twenty packets a second, and the fix
  was to make interrupts work rather than tune the polling. Start polled;
  do not finish polled.
- **The PCI domain is `0001:`, not `0000:`.** Anything that hardcodes domain
  zero will not find this chip.
- **DMA comes from `cma=32M`**, not a `memmap=` reservation.
