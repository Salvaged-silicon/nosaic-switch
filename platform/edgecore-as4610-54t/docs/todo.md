# AS4610-54T — what is left

In the order it has to happen. Each item says what it is, why it is where it is
in the order, and how you would know it is done.

Nothing on this board has been booted. The code exists — an armhf toolchain
definition, a device tree, a datapath and a platform HAL — and every line of it
is unproven on hardware, so this is the whole bring-up rather than a list of
gaps in a working port.

---

## 0. Rack the spare unit

There are two AS4610s. The one in the rack is the lab's out-of-band switch —
the console server hangs off `ge25` and it holds `10.10.200.1` — so installing
on it removes console access to every other board this port would be compared
against, including the AS5610 it is the sibling of.

The spare is not racked, has no console recorded, and its variant has never
been confirmed off the label. **Nothing else on this list can be verified until
it is on a bench with a console.**

*Done when:* the spare is powered, its console is reachable at 115200, and
`onie-sysinfo -p` on it says `arm-accton-as4610-54-r0`.

---

## 1. Build the armhf toolchain

```sh
make toolchain-seed ARCH=armhf
make toolchain      ARCH=armhf
make toolchain-test ARCH=armhf
```

This is `arch/powerpc`'s spike S1 again, and it asks the same two questions:
does a current gcc and glibc build for this target at all, and does the
instruction audit come back clean.

The specific risk is NEON. The upstream sample is named for it, this CPU does
not have it, and `bootstrap/build.sh` overrides `CT_ARCH_FPU="vfpv3"` to
compensate. Read the prediction in `arch/armhf/arch.yml` before interpreting an
audit failure: glibc's ifunc string routines may legitimately contain NEON that
this CPU never dispatches to, and the answer to that is to narrow the audit, not
to widen the FPU.

*Done when:* `toolchain-test` reports a 32-bit little-endian binary that runs
and `instruction audit: 0 forbidden`, and `arch/armhf/arch.yml` moves from
`planned` to `canary` with what was measured written into its notes.

---

## 2. Boot a kernel over TFTP, writing nothing

```sh
make pkg PKG=linux ARCH=armhf
make image BOARD=edgecore-as4610-54t     # it will warn about the datapath; expected
```

then RAM-boot the FIT from the U-Boot prompt as
[install.md](install.md#trying-it-without-writing-anything-first) describes.
Nothing is written to the disk, so a failure costs a power cycle.

Six things are being proved at once, and each has a known way to fail:

| | |
|---|---|
| the FIT is loadable | digest — this firmware knows crc32 and sha1 only |
| the kernel starts | load address `0x61008000`; DRAM is based at `0x61000000` |
| the console is readable | the UART reference clock is 62.5 MHz, not mainline's 25 |
| both cores come up | `secondary-boot-reg` is `0xffff042c`, not NSP's `0xffff0fec` |
| **the disk appears** | the USB PHY has no mainline driver, and is now known to need one — item 3 |
| the management port appears | `BGMAC_PLATFORM` is necessary and probably not sufficient — item 3b |

*Done when:* a console login, `nproc` says 2, `/dev/sda` has four partitions,
and the management interface passes traffic.

---

## 3. USB, which is now known to need patches rather than luck

`brcm,usb-phy-hx4` has no mainline driver. This port declares `generic-ehci` at
`0x1802a000` and nothing initialises the PHY in front of it, on the hope that
U-Boot's own `usb start` leaves it powered through the handover.

**That hope is contradicted.** BISDN run mainline 6.12 on this board and carry:

```
10-17-usb-phy-phy-xgs-iproc.patch      23 KB   a driver for brcm,usb-phy-hx4
10-19-...-usb-phy-mode.patch          1.5 KB
10-15-usb-ipproc-xgs-hack.patch       5.6 KB   into ehci-platform.c
```

and bind `brcm,xgs-iproc-ehci`, not `generic-ehci`. See
[bisdn-prior-art.md](bisdn-prior-art.md).

The disk is behind that controller, so the expected first-boot outcome is a
console followed by `VFS: Unable to mount root fs`.

Try it anyway — a TFTP boot writes nothing and takes minutes — then take those
three patches. They are GPL-2.0 against the same kernel series, and
`recipes/linux/patches/` already exists. Do not adopt their whole
`ARCH_XGS_IPROC` machine to get them; the USB pieces are separable, and three
of their nineteen patches only widen Kconfig dependencies that `ARCH_BCM_HR2`
already satisfies.

*Done when:* `/dev/sda` is present after a cold boot, not only after a warm one.

---

## 3b. The management port, same story

`CONFIG_BGMAC_PLATFORM` plus `brcm,nsp-amac` is necessary and probably not
sufficient: BISDN carry 320 lines of SerDes bring-up in `bgmac-platform.c`,
`bgmac.c` and `bgmac.h` under their own `brcm,xgs-iproc-amac`, and the vendor's
tree on the running switch uses that compatible too.

Expect no management interface on a first boot; take
`10-10-bgmac-xgs-iproc-changes.patch` when that happens.

Lower than USB in the order only because a serial console is enough to work
without it, and nothing at all is enough to work without a root filesystem.

*Done when:* the management interface appears and passes traffic.

---

## 4. Put the FIT on the boot partition as a file

The board installs today by writing the FIT into a raw partition and reading it
back with `usbboot`, because that is the mechanism the image builder supports —
it is what the AS5610 needs, since that board's U-Boot cannot read a filesystem.

**This board's firmware can.** It has `ext2load` and `ext2ls`, and the vendor's
own boot command is `ext2load usb 0:1 $onl_loadaddr $onl_itb`. The builder
already creates an ext2 boot partition and puts the slot pointer on it; what it
cannot do is place a FIT there as a file.

Worth doing for two reasons beyond tidiness. It frees the board to use GPT —
this U-Boot has full EFI partition support, unlike the AS5610's — which gets
back named partitions and the initramfs's preferred way of finding the data
partition. And a FIT that is a file is a FIT that can be replaced without
`dd`-ing a partition, which matters the first time a kernel needs swapping on a
switch that is a long way away.

*Done when:* `imgbuild` writes the FIT onto the boot partition, `board.yml`
drops `fit_mib` and moves to `partition_table: gpt`, and `u_boot_nos_bootcmd`
becomes an `ext2load`.

---

## 5. `fw_setenv` has nothing to write to

The QSPI node is disabled in the board's device tree, because mainline has no
driver for this SoC's AXI clock tree and a QSPI given the wrong bus clock reads
plausible rubbish out of the bootloader you depend on to recover the switch.

The consequence is no `/proc/mtd`, so the running system cannot write the
U-Boot environment: it can neither point the firmware at a new OS nor ask for
ONIE on the next boot. Both currently need the console.

Two routes. Give the QSPI a fixed-rate clock in the device tree once the real
rate is known — it is derived from the AXI clock, which the vendor's
`brcm,xgs-iproc-axi-clk` node computes from a register at `0x1803fc00`, so the
number is readable rather than mysterious. Or write the small clock driver that
node implies, which is a handful of registers and would also bring back the
second UART and the watchdog.

*Done when:* `fw_printenv` works on the running switch, and `nosaic upgrade`
can ask for ONIE without a console.

---

## 6. Get `nosd-helix4` to attach

The datapath exists — `datapath/helix4/`, `recipes/nosd-helix4/` — and
compiles. It has never talked to a chip. It is at this point in the order
because a chip you cannot get the board to boot next to is a chip you cannot
debug.

Start with the probe, which touches no chip state:

```sh
nosd-helix4 --probe
```

It answers four questions in the order they matter: is there a UIO device, does
the register window read something that is a device, is there a DMA pool, and
does the interrupt arm. Only then is the SDK worth involving.

**Three decisions in the source are one line each and a first boot settles
them.** Each is marked in the file it lives in:

1. **The SAL bus type.** `sdk.c` claims `SAL_AXI_DEV_TYPE | SAL_SWITCH_DEV_TYPE`
   because that describes what this is. The uncertainty is that the SDK never
   references that constant anywhere in 6.5.24 — only its own private
   `BDE_AXI_DEV_TYPE`. If attach fails as though a PCI-only path was skipped,
   try `SAL_PCI_DEV_TYPE` instead: the synthesised configuration space is
   already able to serve it. **Write down which one worked.**
2. **Which iProc registers to map.** None are, deliberately: those addresses
   are in the same SoC peripheral space as the kernel's i2c, UART and USB
   drivers, and the disk is behind one of them. The daemon records every one
   the SDK asks for and prints the set at the end of bring-up. Add exactly
   those as a `reg` range on the CMIC node.
3. **Whether the interrupt works.** `interrupt_connect` starts a thread that
   waits on the UIO file descriptor and calls the SDK's handler. If the SDK
   cannot be driven that way, fall back to what the AS5610 does —
   `polled_irq_mode=1`, `schan_intr_enable=0`, `tdma_intr_enable=0`,
   `tslam_intr_enable=0` in `config/asic.conf`, which is why that file
   deliberately says nothing about interrupts today. Do not start there: that
   board pays a core and a punt-latency floor for it.

**The DMA pool has two paths and the tidier one is untried.** `bde.c` prefers
UIO map1 — a second `reg` range on the CMIC node — and falls back to
`reserved-memory` plus `/dev/mem`, which is what the AS5610 does and the only
one of the two that has run anywhere. The probe reports which it got.

**EdgeNOS's `bcmd.c` does not port.** Its packet path is Broadcom's KNET
kernel module and NOSaic has no kernel module on any board. The `bcm_l3_*`
table programming is the same API and should carry almost directly.

*Done when:* front-panel ports appear as `swpN`, two of them pass traffic
between each other in hardware, and the CPU can ping through a third.

---

## 7. Check the platform HAL against the hardware

The HAL exists — `internal/platformhal/as4610/`, driver `as4610-cpld` — and its
decodes are covered by tests against a fake bus. What no test can cover is
whether the register map it was built from is right, and three parts of it are
transcribed from EdgeNOS rather than derived from anything.

In descending order of how wrong they could be:

**The fan tachometers read zero.** On the lab unit the duty register says full
speed and both tachometers read `0x00`, with fans that are certainly turning.
Either the decode is wrong, or those registers need something done first, or
they are not the registers. The driver reports them as read with the raw byte
beside them, and the cooling loop ignores them and works from temperature — so
this is wrong output rather than a hazard, and it should still be settled.

**The fan floor is 50% and the curve starts at 50 °C, both as placeholders.**
Declaring the driver is what starts the cooling loop, so the first boot of an
image built from this takes the fans off full on a chassis whose only known
reading is 49 °C at full speed. Watch a temperature curve against duty, then
lower both. This is the item most likely to be quietly skipped, and the one
where skipping it is a hot switch rather than a wrong number.

**The PSU decode is the vendor's and unchecked.** Pull one power lead and watch
which bit moves. The AS5610's turned out to be two registers rather than two
fields of one, with presence active low, and its own vendor documentation had
it wrong.

**The identity EEPROM has never been read.** `Board()` decodes ONIE TlvInfo,
which is what an ONIE whitebox ought to carry at that address — inference, not
observation. It refuses cleanly and says what it found if the part holds
something else.

**Transceiver pages cannot be selected**, because six cages behind one mux all
answer at `0x50`. SFPs report everything; QSFPs lose their vendor strings. Doing
better needs a page select and a read in one uninterrupted transaction across a
mux channel switch, which the kernel's mux does not offer.

*Done when:* `nosaic platform status` prints fans, supplies and a temperature
that agree with a thermometer and a power lead, and the floor and the curve are
numbers somebody measured.

---

## 8. Settle the front panel numbering

Three numbering schemes exist and the silkscreen-to-port one is not written
down anywhere in software. Two points have been established in the copper block
and they run *backwards* relative to each other, which rules out both obvious
rules and cannot distinguish between the two remaining candidates.

**One more copper data point almost certainly settles all 48.** Plug something
into a front-panel port whose number you can read, and see which port counts.
It takes twenty seconds and it has been outstanding for months.

Until then nothing should extrapolate: every address in this lab derives from
the port, which is known exactly, and only a human looking for the right cage
depends on the silkscreen.

*Done when:* [hardware.md](hardware.md#the-port-map) states the rule rather
than three observations.

---

## Open questions, none of them blocking

**The GE bitmap has 49 bits for 48 ports.** `pbmp_xport_ge=0x3fffffffffffe`
sets bits 1–49. One logical port in that range is enabled and is not brought
out to a cage. Which, and why, is unestablished; it is copied rather than
trimmed because a bitmap that disagrees with the chip's port configuration
fails bring-up without a useful message.

**`ge11`'s PHY is at `0x0e` where the arithmetic says `0x0c`.** One exception
in forty-eight, unexplained, and the reason `tools/mkphymap.sh` reads the map
rather than computing it.

**The real-time clock is disabled and should probably stay that way.** A DS1307
with a flat backup cell wedges the boot on the i2c mux — no message, and the
fault presents as the mux rather than the clock. The vendor ships a whole
second device tree named `rtcdis` for this. Replacing the cell first is the
only safe way to find out.

**Whether `brcm,hr2` is close enough.** Mainline calls this SoC family
Hurricane 2 and this part is a Helix4's iProc block. The peripheral layout
matches address for address, and the machine descriptor is what brings the L2
cache up, so claiming it is right.

The answer is now partly known and it is "close, and not the whole way".
BISDN add a separate machine — `arch/arm/mach-iproc/`, `ARCH_XGS_IPROC`, 165 KB
— rather than reusing Hurricane 2's, and a `bcm-helix4.dtsi` rather than
mainline's `bcm-hr2.dtsi`. What that buys them is USB, the AXI clock tree, the
AMAC's SerDes and their own PCIe; what it costs is a machine patch for a family
mainline already supports. Building on `ARCH_BCM_HR2` and taking the four
driver patches is the smaller thing to carry, and is what
[bisdn-prior-art.md](bisdn-prior-art.md) recommends trying first.
