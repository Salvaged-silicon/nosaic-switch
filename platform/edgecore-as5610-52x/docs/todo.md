# Edgecore AS5610-52X — what is left

NOSaic runs this board. As of 2026-09-03 it boots unattended to a switch that
forwards in hardware, holds four OSPFv2 adjacencies and one OSPFv3, load-balances
across an ECMP pair, controls its own fans and reports its own environmentals.

What follows is what is *not* done, in the order worth attacking it. The
finished work is kept as struck-through headings rather than deleted, because
the reasoning under them is usually why the next thing works.

Status is in [the README](../README.md); the hardware is in
[hardware.md](hardware.md), read off a running unit. The 7050SX2's equivalent is
[its own todo](../../arista-7050sx2-72q/docs/todo.md), and the two share a
datapath -- `datapath/common` -- so a fix in one often lands in both.

## Done, 2026-09-02 to 09-03

Kept short; each has a commit with the reasoning.

- **Bus mastering on the PCIe bridge.** Every DMA the chip issued was discarded
  one hop upstream, at a root port whose Bus Master Enable nobody had set. Three
  unrelated-looking failures, one cause. Everything below waited on it.
- **The front panel.** SFP TX_DISABLE, the QSFP control expander that goes the
  other way, and the DS100DF410 retimers whose CDR reset is not optional.
- **Per-port service VLANs and STG forwarding.** Cumulus's layout, and the
  spanning-tree group nobody sets: a port forwarding in the default group is
  still blocking in its VLAN's own, and Trident+ fires no counter when it drops
  for that reason.
- **CPU punt, taps, hardware L3, ECMP.** `l3sync` reads routes over netlink,
  because `/proc/net/route` cannot express multipath; the ECMP hash has to be
  told what to look at or the group sends everything down one member.
- **The CLI, in C.** Go has no 32-bit big-endian PowerPC target, so this board
  had no `nosaic` at all. `cli/` provides `platform status` and
  `platform thermal` and refuses the rest by name.
- **Cooling.** The board powers up at 31/31 forever; it now tracks temperature
  and idles at the floor.
- **The front-panel port LEDs.** Both LED processors come out of reset halted;
  `datapath/tdp/led.c` loads a passthrough microcode and drives the chain from
  link state, showing the same dark / green / amber the 7050SX2 does. The trap
  worth remembering is that the chain RAM **cannot be read back** while the
  processor is running -- diffing against a read made the panel appear to flap
  on three ports while the SDK reported no link change at all. See
  `docs/hardware.md`.

## Required — nothing works without these

### ~~The board boots to a login~~ — done, 2026-09-02

Every service comes up: `s6rc-oneshot-runner dropbear getty-console ospf6d
ospfd zebra frr-dirs frr-siteconf network nosd`, a getty on ttyS0, and a login
as `admin`. `/proc/mtd` lists `onie`, `u-boot-env`, `board_eeprom` and `uboot`,
so the OS can reach the bootloader environment and therefore ONIE.

What stopped it was s6 using an infinite deadline that does not fit a 32-bit
`time_t`; both `s6-rc-init` and `s6-rc` now get a finite `-t`. Details and the
wrong turn taken first are in [install.md](install.md).

`-D_FILE_OFFSET_BITS=64` is set for every 32-bit architecture and is **not**
what fixed this -- it was added on the wrong diagnosis and kept because it is
correct for a 32-bit target anyway. It is still an ABI-wide switch rather than
a per-package one: `off_t` and `ino_t` change size, so a library built one way
and a program built the other disagree about `struct stat` silently.

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

### ~~The ONIE backend does not handle U-Boot platforms~~ — built, 2026-09-03

The installer now produces something this board can boot. It is **not yet
proven on the hardware**: it has been run end to end against a file, where it
wrote the disk, placed the FIT and set the firmware variable correctly, but no
switch has been installed with it.

**The GPT question is settled, by evidence rather than inference.** The note
below reasoned that a boot command reading `${usbdev}:5` -- a *logical*
partition, which only MBR has -- was "near proof" of no GPT support. It is now
proof: `/dev/mtd3` is the U-Boot image and is readable from the running OS, and
the binary contains no EFI, GUID or GPT strings at all -- only
`## Unknown partition table`. It does carry `ext2load`, `fatload`, `usbboot`
and `usbiddev`. So the board declares `partition_table: dos` and the disk
builder emits a DOS table for it.

**The FIT now has somewhere to live.** `fit_mib: 24` gives it a raw partition
of its own, first on the disk, and the installer writes the image into it after
the table. Raw rather than a filesystem because that is what this firmware
does: `usbboot` is a block read, and `ONIE_ISSUES.md` issue 12 records an
earlier attempt to use `ext2load` here and why it does not match the chain.
A DOS table has four primaries and the FIT takes one, so there is no separate
boot partition and the slot pointer moves onto the data partition -- still
persistent, so A/B still means something.

**One FIT, not one per slot.** An upgrade replaces the kernel for both slots,
so a rollback returns the previous *root filesystem* under the current kernel.
That is a real limitation and it is written down rather than papered over; two
FIT partitions need either an extended partition or a firmware that can pick
between them.

**What the ONIE docs cost, and saved.** `newnos/installer/ONIE_ISSUES.md` and
`newnos/BOOT.md` are the record of doing this once already, and reading them
changed the code three times: ONIE's busybox has no `partprobe` (`sync; sleep 2`
instead), its `fw_setenv` prompts and silently writes nothing unless fed `y`,
and `onie_boot_reason` must be cleared or the box installs perfectly and comes
straight back to the installer. The worst of them was ours: the boot command
was being interpolated into the installer inside double quotes, so the
installer's own shell expanded `${usbdev}` -- unset under ONIE -- and would
have written `usbboot 0x10000000 :1` into the firmware. Both values are now
assigned once, single-quoted, and the build refuses a value containing a quote.

**The safety net is real.** Only `mtd1` (`u-boot-env`) is writable; `onie`,
`uboot` and `board_eeprom` are read-only in the device tree, so nothing the OS
does can damage the bootloader or the recovery image. And `bootcmd` is
`check_boot_reason; nos_bootcmd; onie_bootcmd` -- a `nos_bootcmd` that fails
falls through to ONIE on its own. The dangerous case is not a broken install,
it is an image that boots and then does not work.



**Meanwhile there is a way to run on this board that needs none of it.** The
build now emits a netboot FIT for any board that declares U-Boot addresses,
whatever its installer, and TFTP into RAM writes nothing at all. See
[install.md](install.md). That is the analogue of how the 7050SX2 was first
booted, and it is the right order: prove the image runs before designing the
partition table it will live in.

### ~~FRR cannot write its log~~ — gone

It was FRR running on its packaged default configuration, because nothing
installed the board's own frr.conf. Shipping that fixed it: our configuration
logs to syslog and never opens /var/log/frr.log. The remaining line from that
boot is

```
ZEBRA: Disabling MPLS support (no kernel support)
```

which is a kernel option this board's fragment does not set. Whether MPLS is
wanted on a Trident+ switch is a real question rather than an oversight to
correct blindly, so it is left until someone answers it.

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

### ~~OSPF has no interfaces to run on~~ — done, 2026-09-02

Four adjacencies, on taps created by nosd-tdp. What follows is what it looked
like before the datapath existed.


Verified over ssh on the running board:

```
hostname nosaic-as5610
OSPF Routing Process, Router ID: 10.101.101.241
show ip ospf interface brief -> No such interface name
```

The configuration is right and loaded; there is simply nothing to run it on,
because every OSPF interface is a front-panel port and those come from a
datapath daemon that does not exist. This is the same blocker as `nosd-tdp`,
recorded separately because it is what "NOSaic replaces EdgeNOS here" actually
waits on -- the control plane is done.

### ~~nosd-tdp — the datapath~~ — done, 2026-09-02

It forwards, punts, routes in hardware and load-balances. The scoping below is
kept because it is what made the estimate right.


This is the one remaining thing between "NOSaic boots here" and "NOSaic
replaces EdgeNOS here". Nothing forwards, no front-panel port exists, and OSPF
has nothing to run on. Three questions had to be answered before it could even
be scoped, and all three are now answered, on this SDK, without hardware.

**Does OpenBCM 6.5.24 support this chip?** Yes. `include/soc/devids.h:842`
defines `BCM56846_DEVICE_ID 0xb846`, and `src/soc/esw/drv.c:5038` handles it in
the same cases as the rest of the Trident+ family. The register and memory
database is `src/soc/mcm/bcm56840_a0.c` — the family base, shared by the
whole 5684x line, which is why grepping for the exact part number finds only
eight files and looks discouraging. EdgeNOS reached the same conclusion from
the other direction: its datapath includes `bcm56840_a0_defs.h`.

**Does the SDK build big-endian?** Yes, and it has a PowerPC platform for it.
`make/Makefile.linux-bmw-2_6` is Broadcom's own PPC big-endian target:

```
CROSS_COMPILE = powerpc-wrs-linux-gnu-...
CFGFLAGS += -DSYS_BE_PIO=1 -DSYS_BE_PACKET=0 -DSYS_BE_OTHER=1
ENDIAN = BE_HOST=1
```

**Does it configure with our toolchain?** Yes. `make platform=bmw-2_6 -n` with
`CROSS_COMPILE=powerpc-nosaic-linux-gnu-` resolves and emits exactly
`-DBE_HOST=1 -DSYS_BE_PIO=1 -DSYS_BE_OTHER=1 -DSYS_BE_PACKET=0`. That last set
is worth pausing on: EdgeNOS's switchdb carries "BE PIO needs SYS_BE_PIO=1" as
a hard-won quirk, and it turns out to be what the SDK's own PowerPC platform
sets. The bench found what the vendor already documented in a makefile.

So the work is ordinary rather than speculative:

1. ~~**Build openbcm for powerpc.**~~ **Done, 2026-09-02, first attempt.**
   `openbcm_6.5.24_powerpc.nos`, 13,146 files, 172 static archives — the same
   count as the x86_64 build:

   ```
   bcm56840_a0.o, bcm56840_b0.o   ELF 32-bit MSB relocatable, PowerPC
   libsoc_mcm.a 774M   libbcm.a 36M
   104 SDK defines: -DBE_HOST=1 -DSYS_BE_PIO=1 -DSYS_BE_OTHER=1
                    -DSYS_BE_PACKET=0 -DBCM_ALL_CHIPS
   ```

   The Trident+ chip database is compiled in and big-endian. Checked rather
   than assumed, because the recipe's own comment warns that this target can
   succeed having compiled nothing — hence the archive count, the object
   listing and the `file` output above.

   **What it does not prove:** that any of it runs. This is a compile and link
   for the right architecture with the right chip, and nothing has touched
   silicon. The defines matter beyond this build — anything linking against
   these libraries must compile with the same 104, or `soc_cm_device_vectors_t`
   shifts and every memory ID changes, with no diagnostic of any kind.
2. **A CMICe BDE — and the harder half is PowerPC, not CMICe.**
   `datapath/td2p/bde.c` is written for the 7050SX2's CMICm, and this chip has
   a CMICe whose interrupt and DMA paths differ. That much was expected. What
   was not is this, from EdgeNOS's `asic/edged/bde_interface.c`:

   > Cumulus's switchd accesses BAR0 via `/dev/mem` mmap. We have the mmap set
   > up in `bar0_map` but use ioctl here because plain userspace stores miss
   > the PPC MMIO barriers + endianness that the kernel's `ioread32`/
   > `iowrite32` applies. **Tried direct mmap once — broke S-Channel within
   > seconds.**

   NOSaic's whole BDE strategy is a userspace one over an mmap'd BAR, chosen on
   the 7050SX2 to avoid owning a patched kernel module. On this board that is
   the path that failed. The difference is the architecture, not the chip: on
   PowerPC an MMIO store needs an explicit `eieio` and the byte order is not
   the CPU's, and a plain `volatile` store provides neither.

   It is not a dead end — Cumulus does exactly this from userspace, so it
   works. EdgeNOS records what it would take: explicit `eieio` barriers and
   confirming the **PAXB endianness register at `BAR0+0x2030`**. That is a
   handful of inline-asm lines and one register to establish, not a redesign.
   But it has to be done deliberately, and "broke S-Channel within seconds" is
   the failure mode to expect if it is not.

   Worth noting the SDK is already on this side of the argument: the PowerPC
   platform sets `SYS_BE_PIO=1`, which is precisely the statement that
   programmed I/O to this chip is big-endian.

   **Settled by measurement, 2026-09-02.** `spike/cmic-probe.c` and a `devmem`
   read of the live board:

   ```
   BAR0 physical       0xa0000000  (0001:01:00.0, device 0xb846)
   CMIC_ENDIAN_SELECT  0x04040404
   CMIC_REVID_DEVID    0x46B80200   byteswapped 0x0002B846
   ```

   `0x0002B846` is device `0xb846` revision 2. **A plain volatile read of an
   mmap'd BAR from userspace reaches this chip and returns the correct
   register**, byte-swapped. So userspace access is not the problem and
   EdgeNOS's revert to ioctl was not because the BAR is unreachable.

   The chip is in little-endian PIO today, which is why the kernel's
   `ioread32` — `le32_to_cpu` on a big-endian host — gives `edged` the right
   answer where a raw load does not. Endianness is therefore a choice: set
   `ES_BIG_ENDIAN_PIO` and let the chip present host order, or leave it and
   swap in the accessor.

   **The decision is to keep the userspace BDE**, and it is now demonstrated
   rather than argued. On the board netbooted into NOSaic, with nothing driving
   the chip, `tdp-probe --set-endian`:

   ```
   CMIC_ENDIAN_SELECT (0x174)  0x00000000  (power-on default)
   CMIC_REVID_DEVID   (0x178)  0x46b80200  -> device 0x0200 rev 0xb8
   ... writing CMIC_ENDIAN_SELECT and retrying
   CMIC_ENDIAN_SELECT (0x174)  0x07070707
   CMIC_REVID_DEVID   (0x178)  0x0002b846  -> device 0xb846 rev 0x02
   PASS
   ```

   A userspace BDE over an mmap'd BAR reaches this chip on PowerPC, correctly,
   with the chip in big-endian PIO and `datapath/common/mmio.h` supplying the
   ordering the kernel's `ioread32` would otherwise have supplied. No kernel
   module, and the same BDE model as the 7050SX2.

   The register reads back with its low byte replicated across all four — 0x07
   written, 0x07070707 read — which also explains what a live EdgeNOS shows:
   `0x04040404` is `ES_BIG_ENDIAN_DMA_OTHER` alone, PIO left little-endian
   because the ioctl path swaps for it.

   **The DMA pool works too**, which was the other prerequisite and needed a
   different answer from the 7050SX2's. That board reserves memory with
   `memmap=` on the kernel command line; `memmap=` is an x86 parameter and does
   not exist on PowerPC, and CMA is not available either — this kernel has
   `CONFIG_CMA` but PowerPC has no `HAVE_DMA_CONTIGUOUS`, so `DMA_CMA` cannot
   be selected and `CmaTotal` reads 0.

   The answer is a device-tree `reserved-memory` node with `no-map`, which the
   BDE finds by reading `/proc/device-tree/reserved-memory` rather than being
   told an address — so the daemon and the device tree cannot disagree about
   where the pool is. `no-map` has a second effect worth knowing: the region
   stops being System RAM, so `devmem_is_allowed()` permits mapping it through
   `/dev/mem` even with `CONFIG_STRICT_DEVMEM=y`, which this kernel sets.

   ```
   reserved-memory:  nosaic-dma@78000000
   dma pool     64 MiB at 0x78000000
   dma readback OK
   ```

   **Still unproven: ordering under load.** This is one register at a time and
   S-Channel is a sequence. The barriers exist for it; only traffic will say.
   A kernel shim over `ioread32` remains the fallback, and there is now much
   less reason to expect needing it.

3. **Most of EdgeNOS's chip-init work does not carry over, and that is good
   news.** `edged` links **OpenMDK**, not the full OpenBCM SDK, and OpenMDK
   omits chip initialisation the full SDK performs. That is what
   `asic/edged/cumulus_replicate.c` exists to paper over — four chip memories
   found by cross-correlating register and memory dumps against a live Cumulus
   capture:

   ```
   EPC_LINK_BMAP     egress-pipeline port bitmap
   L2_USER_ENTRY     63 protocol-MAC CPU-trap rules
   EGR_VLAN(_STG)    53 service-VID egress rows + STG state
   FP_TCAM/POLICY    100 chip-side trap rules
   ```

   That is the single largest piece of reverse engineering in EdgeNOS's
   datapath, it is still incomplete, and NOSaic should not inherit any of it.
   Using the vendor SDK to get the initialisation hand-reproduction cannot
   match is the argument the project plan already makes for the 7050SX2; this
   board is the second piece of evidence for it.
4. ~~**The SDK attaches to a vector table it accepts, then segfaults.**~~
   **soc_attach completes, 2026-09-02.**

   ```
   config       119 properties from /etc/nosaic
   CMIC_REVID_DEVID   0x0002b846  -> device 0xb846 rev 0x02
   dma pool     64 MiB at 0x78000000     dma readback OK
   === handing the chip to the SDK ===
   ATTACHED  soc_attach completed
   ```

   **Which also settles the ordering question.** `soc_attach` is chip
   initialisation proper and drives S-Channel — the multi-write sequence whose
   failure EdgeNOS described as "broke S-Channel within seconds". It completed.
   So the barriers in `datapath/common/mmio.h` hold under a sequence and not
   merely one register at a time, and the userspace BDE needs no kernel shim on
   this architecture.

   The last thing in the way was one character per line. EdgeNOS's config.bcm
   writes `portmap_1.0=65:10`, because the SDK's own config.bcm parser
   understands `name.unit`. Nothing parses that when the SDK asks
   `config_var_get` instead: it asks for the bare name, the unit implied by the
   device. So every suffixed property read as unset, and an unset port map is
   what the walk in `soc_counter_attach` faulted on. The 7050SX2's files have
   no suffixes anywhere, which is what pointed at it.

   `sal_config_refresh: cannot read file: config.bcm` is still printed and is
   not a problem: the SDK looks for the file, does not find one, and takes
   every variable through the vector instead.

   *Old text follows, for the record of how it was diagnosed.*

   **The SDK attaches to a vector table it accepts, then segfaults.**
   Where this stands as of 2026-09-02, on the board:

   ```
   CMIC_REVID_DEVID   0x0002b846  -> device 0xb846 rev 0x02
   dma pool     64 MiB at 0x78000000     dma readback OK
   === handing the chip to the SDK ===
   sal_config_refresh: cannot read file: config.bcm, variables not loaded
   Segmentation fault
   ```

   The first attempt returned `SOC_E_PARAM` from `soc_cm_device_init`, which is
   what a vector table missing something required looks like. Adding
   `config_var_get`, `interrupt_connect`/`disconnect`, `sflush`/`sinval`,
   `read64`/`write64` and the three `big_endian_*` flags got past it — so the
   table is now accepted and the crash is inside `soc_attach`, which is chip
   initialisation proper.

   `big_endian_pio`, `big_endian_packet` and `big_endian_other` are the ones
   that make this board different from the 7050SX2, which sets all three to 0.
   They must agree with what was written to `CMIC_ENDIAN_SELECT`, and
   disagreement is not an error the SDK reports — it byte-swaps or fails to,
   and everything is wrong in the same direction.

   **Diagnosed, and it is the configuration rather than the barriers.** The
   kernel logs enough to place it exactly, with no debugger needed:

   ```
   tdp-probe[521]: segfault (11) at 1ec8d3c nip 1007e490 lr 1007e46c
                   in tdp-probe[10000000+3e9d000]
   code: ... 3d2801ed <80e98d3c> ...
   ```

   The binary is mapped at `0x10000000`, so the fault is at offset `0x7e490`,
   which `addr2line` against an unstripped build of the same sources puts in
   **`soc_counter_attach`, `src/soc/common/counter.c:7974`**. The faulting
   instruction is `lwz r7,-29380(r9)` after `addis r9,r8,0x1ed` — a base
   register plus a large constant, and the fault address `0x01ec8d3c` says the
   base was zero.

   Line 7974 is `blk = SOC_PORT_BLOCK(unit, phy_port)`, inside

   ```c
   /* We can't use pbmp_valid calculations, so we must do this manually. */
   for (phy_port = 0; ; phy_port++) {
   ```

   an **unbounded** loop that stops only when the port table reports the end of
   the list. With no port configuration loaded there is no terminator and it
   walks until it faults. That is the SDK behaving reasonably given nothing to
   work from, and it matches the line it printed first: `sal_config_refresh:
   cannot read file: config.bcm, variables not loaded`.

   So the barriers are not implicated, and the next step is not debugging.

   **What it needs is what the 7050SX2 already has.** That board carries
   `config/asic.conf` — Broadcom `config.bcm` content, one property per line —
   and `props.c` answers `config_var_get` from it. This board needs the same
   two things, plus the port map, which the SDK cannot bring up a port without:
   EdgeNOS has a 3.2 KB `config.bcm` for the AS5610 in `/etc/edged/`.

   `props.c` is the third thing both datapaths want and neither should own — a
   properties file reader is not ASIC-specific. `datapath/common/` already
   holds `mmio.h` for the same reason.

5. ~~**A RAM-booted image can read a large file wrong.**~~ **Fixed.**

   `switch_root` deletes the initramfs to free the memory it occupies, and it
   deleted the file the loop device was reading the root filesystem out of. The
   loop device keeps the inode alive so this mostly worked, and mostly was the
   problem: a 125 MB binary read back with pages of zeros in the middle,
   deterministic enough to crash in the same place and transient enough that
   dropping caches and reading again returned the correct bytes.

   The image is now moved onto its own tmpfs before it is mounted. A separate
   tmpfs is a separate filesystem and `switch_root` does not descend into
   those, so the file stays where the loop device left it. Five consecutive
   reads on the board now match the package byte for byte.

   A tree-in-the-initramfs approach was tried first and is worth recording as
   the wrong answer: it removes the loop and the squashfs, but the lower
   directory then lives in the initramfs that `switch_root` is about to delete,
   and the board panics with `Attempted to kill init! exitcode=0x00000100`. The
   loop device was never the problem -- it was the only thing holding the file
   together.

   *Old text follows, for the record of how it was found.*

   **A RAM-booted image can read a large file wrong, and it blocks everything
   above it.**

   `/usr/sbin/tdp-probe` is 125 MB. Read from the running switch it does not
   match the package it was built from, and the difference is pages of zeros
   where code should be:

   ```
   md5 on the box   24ebc77043c05d66c2805da9e6ad6120
   md5 in the image 6e446d72f55def6d229ecd2895d2a679
   bytes at 0x29f4004:  00 00 00 00 00 00 00 00   (should be 3d 20 17 d6)
   ```

   It is not the file and not the transfer. The squashfs on the build host
   holds the correct bytes, and U-Boot verified the FIT's crc32 over the
   initramfs that contains it. It is not decompression either, because
   **dropping caches and reading again gives the correct md5**. Nothing is
   logged: no squashfs error, no OOM, 1.88 GB free.

   So it is the page cache, in the stack a RAM boot builds: a squashfs file
   inside an initramfs unpacked to tmpfs, attached to a loop device, mounted,
   and an overlay on top. A disk-installed image mounts squashfs from a
   partition and has none of that.

   The symptom above it is a jump into a zero page:

   ```
   tdp-probe[522]: illegal instruction (4) at 129f4004 nip 129f4004
   code: ... 00000000 <00000000> 00000000 ...
   ```

   which reads as an unsupported opcode on a soft-float e500v2 — the one thing
   this architecture is expected to produce — and is nothing of the kind. It
   cost a round of chasing a toolchain problem that did not exist.

   **Everything below is untrustworthy until this is fixed**, because a test
   that fails may have read a corrupt binary. Two candidate directions: carry
   the root filesystem in the initramfs as a plain cpio so it unpacks into
   tmpfs with no loop and no squashfs — more RAM, one layer instead of four —
   or find the actual bug, which is worth doing since disk-booted boards share
   squashfs even if not the loop.

6. **Bring-up reaches `bcm_attach`.** With a trustworthy image, most of the
   sequence passes:

   ```
   ATTACHED  soc_attach completed
     soc_misc_init...        ok
     soc_mmu_init...         ok
     bcm_attach...
   nosd-tdp: bcm_attach(0) returned -2 (Out of memory)
   ```

   `soc_misc_init` and `soc_mmu_init` both pass now, which they did not before
   the RAM-boot fix — so the timeout recorded below was very likely the
   corruption rather than anything about the chip. Worth remembering the next
   time something here looks like a hardware fault.

   `bcm_attach` returning `BCM_E_MEMORY` is not our DMA pool: `salloc` reports
   exhaustion by name and said nothing, and the box has 1.8 GB free. So it is
   an allocation or a lookup inside the SDK's own BCM layer. Two things to try,
   cheapest first: pass `"esw"` explicitly rather than NULL for the driver
   family, and check whether this SDK build registers the Trident+ BCM driver
   at all — `soc_attach` succeeding only proves the SOC layer knows the chip.

   **The DMA bisect is clean now, and DMA is the real blocker.**

   ```
   DMA on   soc_misc_init(0) returned -9 (Operation timed out)
   DMA off  soc_misc_init ok, soc_mmu_init ok, reaches bcm_attach
   ```

   The chip is not getting what the CPU writes into the pool. Two explanations
   ruled out:

   - **Addressing.** The P2020's inbound ATMU window for pci1 reads enabled,
     2 GB, targeting local memory with a zero base — 1:1 over DRAM, so
     `0x78000000` is reachable as written. `l2p` returns CPU physical, which is
     what that mapping makes correct.
   - **CPU cache.** `sflush` and `sinval` were no-ops on the argument that a
     `no-map` region mapped through `/dev/mem` with `O_SYNC` is uncached.
     Implementing them properly with `dcbf` over the cache line changed
     nothing, so either the mapping was already uncached or the cache was never
     the problem. The implementations are kept: they are correct either way and
     the no-op version was an assumption rather than a finding.

   **The SDK's own log now says what fails**, which it could not before because
   nosd-tdp discarded it. `bsl_init` is wired up and everything is let through:

   ```
   base: 44c addr: 44c, block: -1, index: 0, pindex: 0, gransh: 2   (x23)
   SlamDmaTimeOut:_soc_xgs3_mem_slam, Abort Failed
   soc_mem_write_range: write CPU_COS_MAP.ipipe0[0-127] failed: Operation timed out
   soc_mem_clear: unit 0 memory CPU_COS_MAP.* returns Operation timed out
   ```

   A SLAM DMA that never completes, and whose abort also fails. Everything
   before it — chip identification, the DMA pool, `soc_attach` — is fine.

   **Three explanations ruled out, each with evidence rather than argument:**

   | Ruled out | How |
   |---|---|
   | Addressing | pci1's inbound ATMU window reads enabled, 2 GB, local memory, zero base — 1:1 over DRAM, so `0x78000000` is reachable as `l2p` reports it |
   | CPU cache | `sflush`/`sinval` implemented with `dcbf` over the cache line; no change |
   | DMA byte order | Set to match the vendor OS running on this board with working DMA — `CMIC_ENDIAN_SELECT` `DMA_OTHER` alone, packet DMA little-endian, `big_endian_packet=0` to agree; no change |

   The endianness one is worth keeping even though it fixed nothing: setting
   all three was a guess that the chip should match the host, and matching the
   machine that works is better grounded than a guess.

   **EdgeNOS wrote down why it uses a kernel module, and it is the best map of
   what is left.** `asic/edged/bde_interface.c` opens with four reasons raw
   `/dev/mem` was not enough on this board:

   | Their reason | Where NOSaic stands |
   |---|---|
   | PPC needs eieio/sync barriers for MMIO; `ioread32` has them | **Answered.** `datapath/common/mmio.h`, and `soc_attach` completing is the proof |
   | CMIC registers above `0x10000` need proper PIO access | **Unexamined.** The BAR is 256 KB and the accessors reach the whole of it, but "proper" is doing work in that sentence |
   | DMA coherent memory must come from the kernel DMA API | **Partly.** A `no-map` reserved region is not what `dma_alloc_coherent` returns, and the difference has not been characterised |
   | IRQ handling for DMA completion | **Not answered.** There is no interrupt delivery at all |

   Two of those four are live candidates for the SLAM DMA that never completes,
   and neither is a guess: they are what the people who got this chip working
   said mattered.

   **Ruled out so far**, each on the board rather than by argument: the inbound
   ATMU window (enabled, 2 GB, 1:1); CPU cache (`dcbf` in `sflush`/`sinval`);
   DMA byte order (matched to the working machine); and the pool's location
   (moved from `0x78000000` in highmem to `0x28000000` in the Normal zone,
   since `dma_alloc_coherent` allocates from Normal and a pool no kernel
   allocator would hand out is a poor place to be — kept regardless, because
   being where the working mechanism would have put it is worth having).

   `polled_irq_mode=1` is also kept: with no interrupt delivery, redirecting
   the SDK's handler thread to poll is correct whatever else is wrong. It
   introduces a segfault on the failure path, after `soc_misc_init` has already
   returned, which is worth knowing and is not the thing being chased.

   **Interrupts are not it, and the register path is not it either.** Both
   checked rather than assumed:

   - The SDK's SLAM wait is a **poll**, not an interrupt wait — it reads
     `CMIC_SLAM_DMA_CFG` in a loop for `DONEf`/`ERRORf` (`mem.c:8670`). Which
     the log confirms: `addr: 44c` repeated is that register being read, and
     `CMIC_SLAM_DMA_CFG` is `0x44C` (`cmic.h:592`). So EdgeNOS's fourth reason
     does not apply to this path.
   - Those registers are at `0x444`–`0x44C`, far below the `0x10000` their
     second reason is about, and the BAR is 256 KB.
   - **Register writes land.** `tdp-probe` now writes a pattern to
     `CMIC_SLAM_DMA_ENTRY_COUNT` and reads it back: `wrote 0x00000055 read
     0x00000055 OK`. Reads were already proven by the chip identifying itself;
     this proves the other direction, which nothing had.

   So the CPU-to-chip register path works in both directions, the poll reaches
   the right register, and the engine is enabled and never reports done or
   error. The failure is inside the chip's SLAM engine or in a precondition for
   running it that is not a register we are getting wrong.

   **Both of those were done, and between them they name the answer.**

   Reading the SDK's non-CMICm SLAM path (`mem.c:8395` onward) shows it does
   use a host buffer — `WRITE_CMIC_SLAM_DMA_PCIMEM_START_ADDRr(unit,
   soc_cm_l2p(unit, buffer))` — allocated through `soc_cm_salloc` and freed
   through `soc_cm_sfree`, so through our pool. `l2p` now complains loudly if
   it is ever handed a pointer outside that pool instead of quietly returning
   0, which would hand the chip physical address zero and look exactly like a
   dead engine. **It never complains.** Every address the chip is given is in
   the pool and correctly translated.

   And the captured Cumulus configuration for this board
   (`newnos/docs/cumulus_capture_2026_06_07/config.bcm`) settles what the only
   known-working software on this chip does:

   | | Cumulus | NOSaic |
   |---|---|---|
   | `polled_irq_mode` | 0 | 1 |
   | `miim_intr_enable` | 1 | 0 |
   | `tdma_intr_enable` | 1 | 0 |
   | `tslam_intr_enable` | 1 | 0 |
   | `tslam_dma_enable` | 1 | 1 |
   | `table_dma_enable` | 1 | 1 |

   Its own comments name what those mean: *"Table SLAM DMA operation should use
   interrupt rather than poll for completion."* Cumulus takes the interrupt
   for every one of them.

   **So the conclusion is that DMA on this chip wants interrupt delivery, and
   NOSaic has none.** The polled path exists in the SDK and is what
   `tslam_intr_enable=0` selects; it reaches the right register, the engine is
   enabled, and `DONEf` never sets. Everything else is verified: registers read
   and write correctly, the addresses are right, the pool is where a kernel
   allocator would have put it, byte order matches the working machine, and the
   cache is handled.

   **A bug of mine was found underneath all of this and is fixed.** The probe
   declared the BDE as a local in `main()` and handed its address to the SDK as
   the device cookie. The SDK keeps that for the life of the device and calls
   back through it from its own threads — and with `polled_irq_mode` there is a
   thread reading interrupt status. Printing the state at the bounds check gave

   ```
   read past BAR0: 0x50 (bde=0xbf8547b8 bar=(nil) bar_len=0 dma_phys=0xfed380c0...)
   ```

   a stack address holding nothing. Every SDK register access was rejected and
   returned `0xffffffff`, which from above looks like a chip that will not
   answer: parity errors dispatched from all-ones status registers, a DMA
   engine that never completes, `bcm_attach` failing to allocate. It is
   `static` now and the rejected accesses are gone.

   **It did not change the DMA failure or the `bcm_attach` error**, both of
   which reproduce with register access verified correct — so the conclusions
   above stand. But it was underneath some of the evidence for them, and
   anything that turns on a single observation from before this fix is worth
   re-checking rather than trusted.

   That makes the CMICe interrupt path the next real piece of work rather than
   one more thing to try — and it is the same piece the BDE has been carrying
   over from CMICm unexamined since the start. Until it exists,
   `table_dma_enable=0` and `tslam_dma_enable=0` are how this board gets
   through `soc_misc_init`, at the cost of programming every table by PIO.

7. **~~`soc_misc_init` times out~~ — passed, see above. Kept for the record.**

   ```
   ATTACHED  soc_attach completed
     soc_misc_init...
   nosd-tdp: soc_misc_init(0) returned -9 (Operation timed out)
   ```

   Ruled out so far. **Bus mastering** is enabled — and the check that said
   otherwise was reading PCI config space without byte-swapping, which is a bug
   in the reader rather than the chip. **Interrupts**: every path that can wait
   on one now polls, `miim`/`tdma`/`tslam`/`schan_intr_enable=0`, because
   `nosd-tdp` delivers no interrupts and the 7050SX2's asic.conf records
   exactly this failure. Neither changed the timeout.

   The bisect that would settle the next candidate — DMA addressing, whether
   the chip sees the pool where we think it does through the PCIe inbound
   window — is `table_dma_enable=0` and `tslam_dma_enable=0`, now set. Its
   first run hit the corruption above and told us nothing, which is why that
   comes first.

7. **`nosd-tdp` itself**, as a sibling of `nosd-td2p` — `provides: [nosd]`,
   `conflicts: [nosd]`. The two share a northbound contract and most of their
   structure; whether they can share code is the first honest test of whether
   the per-ASIC split was drawn in the right place, which is what the project
   plan says this board is for.

Every define the SDK is built with must also be set when compiling against it.
The openbcm recipe already captures them into `sdk-defines.txt` for exactly
this reason: `INCLUDE_RCPU` shifts `soc_cm_device_vectors_t` by a pointer and
`BCM_ALL_CHIPS` changes every memory ID, and neither mismatch produces a
compile error, a link error, or a log line.

### ~~Decide how the SFP status expanders are reached~~ — raw I2C, done

`scripts/sfp-init.sh` drives them over `/dev/i2c-N`, the option this section
called proven. Buses are discovered rather than hard-coded, because the
numbering is a property of the kernel's enumeration order and has already
changed once between versions on this board.


The four expanders carrying **MOD_ABS, TX_FAULT, RX_LOS and TX_DISABLE for
ports 1-48** are declared in `dts/as5610-52x.dts` only as comments, so they
have no Linux devices. They are physically present: a read-only probe of bus 65
ACKs at `0x20`, `0x21`, `0x22` and `0x23`.

**EdgeNOS drives them anyway**, with raw I2C on `/dev/i2c-N` and no device tree
nodes — and does the same for the QSFP control expanders that *are* bound as
gpiochips. So this is a design decision, not a gap: declare them and get
gpiochips and a kernel driver, or use raw I2C, which is proven on this
hardware.

If declaring them: ONLP calls them PCA9506, a 40-pin part, which fits
"ports 0-39" in a way the 8-pin `pca9538` on the neighbouring channel does not.
Worth confirming the marking on the board first. `nxp,pca9505`/`pca9506` needs
`CONFIG_GPIO_PCA953X`, which this kernel now has.

### ~~The platform HAL~~ — answered differently: there cannot be one here

The HAL lives in the Go CLI and Go has no target for this architecture, so the
CPLD is driven from `cli/` in C instead. The two vendor bugs below were both
real and both avoided.


Everything it needs to talk to is written down in
[hardware.md](hardware.md#sensors-fans-and-psus), read off the running unit
rather than inferred: the CPLD at `0xEA000000` with its register map, PSU
decoding, the fan PWM scale, and which temperature sensors are real.

Two things EdgeNOS gets wrong, both worth not copying:

- **The CPLD driver mis-decodes PSU presence.** It reads bit0 of `0x01`
  active-high, so a running switch reports both supplies absent. The correct
  map is PSU1 in `0x02`, PSU2 in `0x01`, present when bit0 is `0`. EdgeNOS's
  own Python HAL bypasses the driver for exactly this reason.
- **`fan_set()` uses the wrong scale.** It writes `pct * 255 / 100` into a
  five-bit field, so 50% becomes 127, whose low bits are 31 — full speed. The
  shell fan controller in the same tree uses the correct 0–31 range, so the two
  disagree.

And one hazard that is not a bug: **`bde_tmon` reads 150 °C on an idle
switch.** It is the Broadcom die's own monitor, not a board sensor. A thermal
loop that includes it runs the fans at full permanently, or halts the box. The
working controller skips it by name and discards anything at or above 120 °C.

What remains is ordinary implementation: NOSaic's `platform-hal` has a driver
interface the 7050SX2 already implements, and this board needs its own — plus
the decision about whether the CPLD is reached through a kernel driver, as
EdgeNOS does, or mapped directly the way the 7050SX2's SCD is. There is no
`platform_hal` stanza in `board.yml` yet, which is why the thermal service is
not generated for this board at all.

### ~~The board's own hardware is undescribed~~ — described and driven

[hardware.md](hardware.md) covers it and `cli/` drives it. The retimer warning
below was exactly right: unprogrammed, the links come up and pass nothing.
LED *firmware* is still not loaded -- see the feature list.


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

## Features — what this board could do and does not yet

Ordered by what a switch is expected to do, not by effort. Anything shared with
the 7050SX2 lives in `datapath/common` and lands on both boards at once; those
are marked *(shared)*.

### Forwarding

- **ACLs.** *(shared)* The one item EdgeNOS never finished either: an entry
  installs into the IFP TCAM, reads back correctly and never matches a packet.
  See "the field processor does not evaluate live traffic" above -- it is a real
  blocker with a recommended next step, not an unknown.
- **VLANs as a user-facing feature.** Ports sit in per-port service VLANs and
  `--bridge` throws every port into one. Neither is a VLAN *model*: there is no
  way to say "these six ports are VLAN 100, tagged on the uplink". This is the
  first thing an operator will ask for and the datapath already has the calls.
- **Link aggregation.** *(shared)* No LACP, no static bonds. The chip does
  trunking and the SDK exposes it; nothing above knows the concept.
- **Storm control, and any policer at all.** Nothing rate-limits broadcast,
  multicast or unknown-unicast, so one loop on a neighbour is this box's
  problem too.
- **MTU.** Taps come up at 1500 and the board's `network.conf` asks for 1600;
  the chip is good for 9216. Nothing plumbs a requested MTU to the port.

### Control plane

- **BGP.** FRR is built with it; nothing configures it and it has never run
  here. OSPF works, so the plumbing underneath is proven.
- **BFD.** Fast failure detection matters much more on a box whose punt path is
  milliseconds; worth doing after ACLs, since CoPP protects it.
- **CoPP.** *(shared)* Nothing protects the CPU from the punt path. A broadcast
  storm arriving on a front-panel port is currently the control plane's problem.
  Blocked behind ACLs, which is most of why ACLs are first.

### The box itself

- **~~Port LEDs~~** — done, see *Done, 2026-09-02 to 09-03*.
- **The status lamps need one pass at the panel.** PS1, PS2, Diag, Fan and Loc
  exist and the installation guide says what each colour means, but **which bit
  of `0x13`/`0x15` drives which lamp is recorded nowhere** -- Cumulus, ONL and
  Accton all expose the registers raw and decode neither, and both registers
  accept all eight bits, so the read-back trick that mapped the 7050SX2's LEDs
  answers nothing. `nosaic platform ledwalk [seconds]` lights one bit at a time
  and restores the registers afterwards; one pass in front of the switch
  produces the map, and then this board can render health the way the 7050SX2
  already does.
- **Per-tray fan status.** `0x03` is read and reported raw. EdgeNOS does not
  decode it either, so there is no known-good map to copy -- it needs working
  out against a box with a tray pulled. The same pass could settle this.
- **Writing LEDs.** Deliberately not implemented rather than guessed.
- **`board_eeprom`.** The board's identity and MAC addresses live in an MTD
  partition nothing reads. A board that cannot read its own MAC gets a random
  one that changes every boot -- fine for a RAM boot, not fine installed.

### The 40G ports link but punt nothing to the CPU

The cages come out of reset now and all three 40G ports carry link -- swp49 to
the Nexus, swp51 to the 7050SX2, swp52 to the 7050TX-64 -- and the panel lights
them. **None forms an OSPF adjacency**, while every 10G port does.

The measurement that defines the fault, taken 65 seconds apart on a working 10G
port and a broken 40G one:

| port | ingress over 65 s | frames reaching Linux |
|---|---|---|
| swp8, 10G | `in-nuc` 6 -> 12 (**+6**) | **17** |
| swp51, 40G | `in-nuc` 7 -> 13 (**+6**) | **0** |

The same hellos arrive at both MACs at the same rate. One port's reach the CPU
and the other's do not.

Direction is established, not assumed: pinging across the link from the
7050SX2, its `et54` tap receives exactly the four ARP requests the AS5610 sent,
so the AS5610 transmits and the far end receives, punts and replies. The
AS5610 never sees the reply, and its own ARP entry for the far end goes FAILED.

Everything above the MAC reads correct, and every one of these was checked
against a working port rather than assumed:

- enabled, linked, `in-err` and `in-disc` both zero
- in its service VLAN with the CPU a member: `vid 3351 members 0 51 untagged 51`
- **forwarding in that VLAN's own spanning-tree group**, `stg=1 stp=4`, which is
  the same pair a working 10G port reads
- not arriving under some other port number: the punted frames nobody claims
  come from exactly three distinct sources, logical 22, 23 and 24, and none of
  them is 49, 51 or 52

**Four explanations were tried on the hardware and all four were wrong**, which
is the useful part of this entry. That the punt header carries a physical rather
than a logical port on a 40G port (matching the SDK's l2p value, swp51 -> 61,
changed nothing). That the incoming number needed translating back through
`port_p2l_mapping` (nothing). That the ports were blocking in their service
VLAN's STG, which is a real trap on this board and is not this (`stp=4`). And
that the `matched no tap` counter was the 40G frames being dropped -- it is a
red herring, its frames come from untapped ports, and it was only convincing
because the log capped at the first eight frames and they all came from one busy
port.

So the question is now narrow: a port that is enabled, linked, forwarding, and a
member of a VLAN the CPU is also in, receives frames at its MAC and floods none
of them to the CPU. The next place to look is the hardware tables rather than
the SDK's view of them -- the VLAN's port bitmap and the flood/CPU membership as
the chip holds them -- and whether a Trident+ 40G port needs anything per-port
before it will punt at all. `--stats` cannot be used for that on a running
switch because it re-initialises the chip, which is the same `switch-api` gap
listed below.

### Operating it

- **Install to flash.** Everything so far is a TFTP RAM boot that writes
  nothing. The ONIE backend does not handle U-Boot platforms yet (above), and
  the A/B slot layout has never been exercised here.
- **`nosaic` beyond `platform`.** The C CLI covers `platform status` and
  `platform thermal`. `show`, `interface` and `route` are mostly a client over
  nosd's socket and would work the same way; they are simply not written.
- **Counters an operator can see.** The chip counts; `--stats` re-initialises
  the chip to read them, so it cannot be used on a running switch. The daemon
  prints a table to its log once a minute, which is not the same thing.
- **Anything that queries a running nosd.** There is no southbound socket yet,
  which is the `switch-api` gap: every diagnostic here either reads a log or
  restarts the datapath.
- **Restarting `nosd` takes the switch off the network, silently.** The taps are
  created by the daemon, so restarting it destroys and recreates them -- without
  addresses and at the default MTU. `apply-network.sh` ran once at boot and
  nothing re-runs it, so the box comes back with every front-panel address gone
  and MTU 1500 against a fabric at 1600. OSPFv2 loses every adjacency and
  OSPFv3 hangs in ExStart, which is the MTU mismatch showing. The s6 dependency
  is declared but s6 does not restart dependents when a dependency restarts.
  Recovering by hand is `sh /etc/nosaic/apply-network.sh` then restarting
  `ospfd`/`ospf6d`; the fix is for the network service to be a consumer that
  re-runs, or for nosd to persist the taps.

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
- **DMA is a `reserved-memory` node, not `cma=32M`.** The command line still
  says `cma=32M` and NOSaic does not use it; the pool is `nosaic-dma@28000000`,
  64 MiB, `no-map`. `no-map` is the load-bearing part: the kernel never maps it,
  so `CONFIG_STRICT_DEVMEM` does not stand between the BDE and the pool.
- **Interrupts are still polled**, and `polled_irq_delay` cannot go below 20 ms
  without the SDK's `sal_usleep` busy-waiting a whole core --
  `datapath/tdp/sdk.c` wraps it so it can. 2 ms costs 1% of a core and gives
  1.7 ms punt latency. Real interrupts would remove the tradeoff and need a
  kernel path to userspace; `uio_pci_generic` is in-tree.
