# Building an image for the AS4610-54T

Nothing has been built for this board. This page is what the build needs and
what of it exists — written before the first attempt so that whoever makes it
is arguing with the hardware rather than with this repository.

The general build — container, toolchain, packages, image — is in
[docs/BUILDING.md](../../../docs/BUILDING.md). Only what is specific to this
board is here.

```sh
make toolchain-seed ARCH=armhf        # ← has never been run; start here
make toolchain      ARCH=armhf
make toolchain-test ARCH=armhf        # ← the gate. Do not skip it.

make packages ARCH=armhf PROFILE=minimal
make pkg PKG=linux ARCH=armhf
make image BOARD=edgecore-as4610-54t
```

## The toolchain is new, and the sample lies about the CPU

`arch/armhf` is written and has never been built. There is no spike behind it
of the kind `arch/powerpc` got, so the first two commands above are the real
first step of this port.

The seed step matters more here than on any previous architecture, and it is
worth understanding why before running it.

crosstool-NG's closest sample is `arm-cortexa9_neon-linux-gnueabihf`, which is
right about the core and wrong about its options. Read off the running switch:

```
CPU part : 0xc09                                    Cortex-A9
Features : half thumb fastmult vfp edsp vfpv3 tls vfpd32
```

No `neon`. No `idiva`/`idivt`. Both are optional on a Cortex-A9 and this part
has neither, so a toolchain built from that sample unaltered emits Advanced
SIMD and hardware divides that this CPU traps on.

`bootstrap/build.sh` overrides `CT_ARCH_FPU="vfpv3"` for this architecture for
exactly that reason, and `arch/armhf/arch.yml` carries an instruction audit as
the backstop. **This is the same trap `arch/powerpc` documents in a different
dialect**: the wrong build compiles, links, disassembles and runs perfectly
under QEMU — whose `virt` machine offers cortex-a7 or cortex-a15, both of which
*do* have NEON — and dies only on the switch.

So `make toolchain-test ARCH=armhf` is the gate, and there is one prediction
written into `arch.yml` that is worth reading before you interpret its output:
glibc on ARM selects `memcpy` and friends through ifunc on `HWCAP_NEON`, so a
correct build may legitimately *contain* NEON in a variant this CPU never
dispatches to. If the audit fires on glibc's string routines and on nothing
else, that is this — narrow the audit, do not turn the FPU back up.

## Go works here, unlike on the AS5610

The gc toolchain has `GOARCH=arm`, so this board runs the **Go** CLI, not the C
one in `cli/`. The AS5610 runs the C CLI because 32-bit big-endian PowerPC is a
target Go has never had, and that is a property of that architecture rather
than of end-of-service-life hardware in general.

Practically: `arch/armhf/arch.yml` sets `go_arch: arm`, `imgbuild` sees a
non-empty `GoArch` and does not pull in `nosaic-cli`, and the full command
surface is available on this switch.

## The kernel needs three things multi_v7_defconfig does not have

`recipes/linux/config/armhf.fragment` is written and, like everything else
here, unbuilt. Three of its entries are the difference between a board that
boots and one that does not, and they are worth knowing separately from the
rest of the file:

- **`CONFIG_BGMAC_PLATFORM`.** The defconfig carries `BGMAC_BCMA`, which is the
  same MAC on a different bus and does not match a device-tree node. Without
  the platform one there is no management interface at all.
- **USB, built in, all of it.** The disk is behind the SoC's EHCI controller.
  A kernel that has USB storage as a module cannot mount the root filesystem
  that the module lives on, and the panic names the filesystem rather than the
  bus.
- **`CONFIG_UIO_PDRV_GENIRQ`.** This is how the datapath reaches the switch
  chip — see [hardware.md](hardware.md#how-nosaic-reaches-the-register-window).
  Without it `/dev/uio0` never appears.

## The device tree is ours, and must not be the vendor's

`dts/as4610-54t.dts` is compiled at image time and carried inside the FIT. It
is derived from the tree this switch boots today — extracted from the vendor's
`.itb` and decompiled — with **every `compatible` string replaced**.

That is the single most important thing to know before editing it. The
vendor's tree names Broadcom BSP drivers that mainline does not have:
`brcm,xgs-iproc-amac`, `brcm,xgs-iproc-armpll`, `brcm,usb-phy-hx4`,
`brcm,helix4`. A kernel built from this repository binds none of them, and the
result is a switch that reaches a console — the UART is a plain `ns16550a` and
needs no driver from anybody — with no network, no USB and therefore no disk.
It looks like a boot that nearly worked.

The file's comments say which nodes are mainline's, which are the vendor's
addresses under mainline's names, and which are disabled because mainline
cannot drive them.

## The datapath

`asic: helix4` resolves to `nosd-helix4`, and `recipes/nosd-helix4/` and
`datapath/helix4/` both exist. It compiles. It has never run.

```sh
make pkg PKG=openbcm     ARCH=armhf     # the SDK, staged not shipped
make pkg PKG=nosd-helix4 ARCH=armhf
```

**It builds without the SDK too, and that is the useful part today.** With no
`SDK_DIR` the makefile builds the BDE and the daemon's probe mode and stubs the
vector layer, which is what CI compiles and what a contributor without the
vendor tree can work on. That binary is worth running on the switch before
anything else:

```
# nosd-helix4 --probe
uio          /dev/uio0  (48000000.iproc_cmicd)
registers    0x48000000, 256 KiB
chip         id register 0x0140b340 -> device 0xb340 rev 0x01
dma pool     0x88000000, 64 MiB, via UIO map1
interrupt    armed; none in 100 ms, which is expected on an idle chip
```

It touches no chip state, so it is safe on a switch that is running, and it
answers the four questions that have to be yes before the SDK is worth
involving. The chip line above is what the register *should* say — `0xb340` is
Helix4's device id — and is the one line in that block nobody has seen.

The chip support behind it is already there. OpenBCM 6.5.24 — the same recipe
the other three datapath packages use — carries Helix4:

```
src/soc/esw/helix4.c        the Helix4 SOC driver
src/soc/mcm/bcm56340_a0.c   the register database
rc/bcm56340sanity.soc       the chip's own sanity script
make/Makefile.linux-iproc-4_14   the IPROC_CMICD platform definition
```

`recipes/openbcm/recipe.yml` already builds for this: its `arch: armhf` block
selects `platform=iproc-4_14`, which sets `-DIPROC_CMICD -DLE_HOST=1` and is
how the SDK is told the chip is not behind a PCI bus. Note that block sets
`CROSS_GCC_VER` explicitly — the SDK's iProc makefile derives it by running a
compiler out of Broadcom's internal toolchain path, which is not here, and the
silent consequence is that every "is the compiler newer than 8" guard fails
open and a decade-old codebase meets gcc 15 with its warning suppressions off.

**What will not carry over from EdgeNOS is the packet path.** Their datapath
uses Broadcom's KNET kernel module; NOSaic has no kernel module on any board.
The forwarding-table code is the same `bcm_l3_*` API and should port almost
directly. The `geN`/`xeN` netdevs and the filters that feed them do not exist
on this side at all — `datapath/common/tapbridge.c` does that job, and it is
shared with the other three datapaths rather than written again here.

### Three lines in it that a first boot will settle

Each is one word or one address, and each is called out in the source:

- **The SAL bus type.** `sdk.c` claims `SAL_AXI_DEV_TYPE | SAL_SWITCH_DEV_TYPE`,
  which describes what this is — and the SDK never references that constant
  anywhere in 6.5.24, only its own `BDE_AXI_DEV_TYPE`. If attach behaves as
  though a PCI-only path was skipped, try `SAL_PCI_DEV_TYPE` instead; the
  synthesised config space is already able to serve it.
- **Which iProc registers to map.** None are, by design. The daemon prints the
  set the SDK asked for, and the board's device tree then maps exactly those.
- **Whether the SDK's handler can be driven from a UIO read.** If it cannot,
  this board falls back to what the AS5610 does — `polled_irq_mode=1` and
  friends in `config/asic.conf`, which is why that file deliberately says
  nothing about interrupts today.

## The platform HAL exists, and it is Go

`internal/platformhal/as4610/`, registered as `as4610-cpld`. Fans, power
supplies, the board sensor, the identity EEPROM, the transceiver cages, and
the five CPLD writes that release the front panel.

Nothing extra has to be built for it: it is part of the `nosaic` CLI, which
`make image` cross-compiles as static Go. `go test ./internal/platformhal/as4610/`
covers the decodes against a fake bus, which is where the LM77's sign and the
fan duty rounding are pinned down.

**Declaring the driver in `board.yml` starts the cooling loop** — that is how
`imgbuild` decides to emit the thermal service. On a chassis nobody has
measured that means the first boot takes the fans off full, which is why the
driver holds a 50% floor and the board's curve starts at 50 °C.

## Two things to expect on a first boot

- **The USB PHY has no driver.** Mainline has nothing for `brcm,usb-phy-hx4`,
  so the EHCI node relies on U-Boot's own `usb start` having left the PHY
  powered through the handover. It may well have. Nobody has watched.
- **`fw_setenv` will not work.** The QSPI node is disabled, so there is no
  `/proc/mtd` and nothing to write the U-Boot environment through. The
  installer's step that points the firmware at the new OS therefore has to be
  done by hand at the U-Boot prompt the first time; [install.md](install.md)
  gives the exact commands.

Both are in [todo.md](todo.md) with what it would take to fix them.
