# Edgecore AS5610-52X — what is left

Everything. Nothing NOSaic has built runs on this board, and this page is the
order to attack it in rather than a list of defects.

Status of the board is in [the README](../README.md); what is known about the
hardware is in [hardware.md](hardware.md), and almost all of it was read off a
unit running EdgeNOS rather than produced here.

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

### OSPF has no interfaces to run on

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

### nosd-tdp — the datapath, and it is not blocked on the unknowns it looked blocked on

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

5. **A RAM-booted image can read a large file wrong, and it blocks everything
   above it.** This is now the first thing to fix.

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

6. **`soc_misc_init` times out, cause not yet established.**

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

### Decide how the SFP status expanders are reached — not whether

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

### The platform HAL — now specified, and two vendor bugs not to inherit

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
