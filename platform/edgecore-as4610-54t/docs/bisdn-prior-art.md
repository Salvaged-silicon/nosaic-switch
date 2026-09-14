# What BISDN Linux already learned about this board

BISDN Linux runs this switch. It is a Yocto distribution for whitebox
switches — `baseboxd`, an OpenFlow controller over Broadcom OF-DPA — and the
AS4610-30T, -30P, -54T and -54TP are supported platforms. Its sources are
public.

It matters here more than most prior art, because BISDN and NOSaic are trying
to do the same unusual thing: **run a current mainline kernel on this board.**
Their `linux-yocto-onl` pulls plain `linux-stable` 6.12.y — the same kernel
series NOSaic builds — rather than a vendor fork. So where they carry a patch,
that is a statement that mainline does not do it, made by somebody who booted
the result.

Read this before debugging a first boot. It confirms four things this port
guessed at, and contradicts two.

Sources, all read on 2026-09-14:
[bisdn/bisdn-linux](https://github.com/bisdn/bisdn-linux),
[bisdn/meta-open-network-linux](https://github.com/bisdn/meta-open-network-linux),
[bisdn/meta-ofdpa](https://github.com/bisdn/meta-ofdpa),
[docs.bisdn.de](https://docs.bisdn.de/).

## Confirmed

Four numbers in this port were read off the hardware and are now corroborated
by an independent implementation, which is worth more than either on its own.

| | Ours | BISDN's |
|---|---|---|
| Kernel load and entry | `0x61008000` | `UBOOT_ENTRYPOINT="0x61008000"` |
| Ramdisk load | left to U-Boot | `UBOOT_RD_LOADADDRESS = "0x00000000"` |
| FIT digest | `crc32` | `FIT_HASH_ALG = "sha1"`, with the comment *"AS4610 U-Boot does not support sha256"* |
| Second CPU | `brcm,bcm-nsp-smp`, `secondary-boot-reg = <0xffff042c>` | identical, character for character |
| Cortex-A9 errata | 754322, 775420, 764369 | the same three, and no others |
| No NEON | `CT_ARCH_FPU="vfpv3"`, and an audit | `DEFAULTTUNE = "cortexa9"`, not `cortexa9-neon`; kernel has `CONFIG_VFP`/`CONFIG_VFPv3` and no NEON symbol at all |

The `secondary-boot-reg` agreement is the most reassuring. That address was
read out of the device tree the switch boots today and is **not** the
`0xffff0fec` mainline's `bcm-nsp.dtsi` uses for the same enable-method; a wrong
one leaves the second core spinning with no message. Two independent sources
now say `0xffff042c`.

The digest line is the one to act on. Both projects independently found that
this U-Boot cannot do sha256 — we by dumping `mtd0` and reading it, they by
hitting it. They chose sha1 and we chose crc32. **sha1 is the better of the two
this firmware has**, and switching is a one-word change to `u_boot_fit_hash`.

## Contradicted

Two of this port's claims are wrong, and both are load-bearing: they are the
disk and the way in.

`arch/armhf` and `dts/as4610-54t.dts` are built on the position that mainline
6.12 has enough of this SoC — `ARCH_BCM_HR2`, `bcm-hr2.dtsi`, and mainline
bindings throughout. BISDN, on the same kernel series and the same board,
carries **nineteen out-of-tree patches, about 440 KB**, and adds a whole
machine directory (`arch/arm/mach-iproc/`, `ARCH_XGS_IPROC`) rather than reusing
Hurricane 2's.

Their device tree is `bcm-helix4.dtsi`, which that patch set adds. It is not
mainline's `bcm-hr2.dtsi`, and the patch modifies that file too.

### The USB PHY is not going to work as written

This port's `hardware.md` calls the USB PHY *"the least certain node in this
file"* and hopes U-Boot's own `usb start` leaves it powered through the
handover. That hope is now contradicted:

```
10-17-usb-phy-phy-xgs-iproc.patch          23 KB   a driver for brcm,usb-phy-hx4
10-19-...-usb-phy-mode.patch              1.5 KB
10-15-usb-ipproc-xgs-hack.patch           5.6 KB   ehci-platform.c + ohci-platform.c
```

Their device tree binds `brcm,xgs-iproc-ehci`, not `generic-ehci`, and
`CONFIG_USB_EHCI_XGS_IPROC` patches `ehci-platform.c` directly.

**The disk is behind that controller.** So the most likely first-boot outcome
is a kernel that reaches a console and then panics unable to mount root — which
[todo.md](todo.md) already predicts, for a reason that turns out to be right.

### The management port probably will not come up either

`recipes/linux/config/armhf.fragment` sets `CONFIG_BGMAC_PLATFORM` and the
device tree claims `brcm,nsp-amac`, on the reasoning that the AMAC is the AMAC.
BISDN's `10-10-bgmac-xgs-iproc-changes.patch` is 320 lines across
`bgmac-platform.c`, `bgmac.c` and `bgmac.h`, and pulls in
`<linux/soc/bcm/xgs-iproc-misc-setup.h>` and `<linux/phy/xgs_iproc_serdes.h>` —
SerDes bring-up this SoC needs and mainline's bgmac does not do. Their
compatible is `brcm,xgs-iproc-amac`.

So `ma1`'s equivalent under NOSaic is likely to be absent or dead. The vendor
device tree on the running switch says `brcm,xgs-iproc-amac` too, which is why
EdgeNOS binds `bgmac-enet` to it — that kernel carries the same patch.

### Smaller ones

- **HIGHMEM.** They set `CONFIG_HIGHMEM=y`; our fragment does not, on the
  reasoning that 768 MB fits in lowmem. It only just does, before the kernel's
  vmalloc reservation. Cheap to add and awkward to diagnose.
- **The RNG.** Ours claims `brcm,bcm-nsp-rng`; theirs is `brcm,iproc-rng100`
  with its own driver (`10-04`). Ours will not bind. Cosmetic — no `/dev/hwrng`
  — except on first boot, when host keys are generated.
- **The AXI clock.** `10-06-clk-clk-xgs-iproc.patch` is the driver for
  `brcm,xgs-iproc-axi-clk`. This port disables the QSPI, the second UART and
  the watchdog for want of exactly that clock, which was the right call and now
  has a known fix.
- **Three of the nineteen are Kconfig one-liners** widening existing mainline
  drivers to `ARCH_XGS_IPROC` (gpio, i2c, mdio). Building on `ARCH_BCM_HR2`, as
  this port does, avoids those three.

## Where the two projects genuinely differ, and neither is wrong

**The datapath.** BISDN runs Broadcom OF-DPA with `baseboxd` translating Linux
networking to OpenFlow. NOSaic drives the chip through the OpenBCM SDK behind
its own northbound contract. Different products, not different opinions about
the same one.

**How userspace reaches the chip, and this is the one to think about.** BISDN
builds the SDK's own GPL kernel modules — `linux-kernel-bde`, `linux-user-bde`,
`linux-bcm-knet` — from
[bisdn/OpenBCM](https://github.com/bisdn/OpenBCM), **SDK 6.5.24, the same
version this repository pins**, against a 6.12 kernel. `CONFIG_UIO is not set`
in their kernel.

`nosd-helix4` does the opposite: no kernel module, a UIO mapping of the CMIC,
and the DMA pool as a second `reg` on that node. Nobody else on this board does
that, so it has no precedent to lean on — which is worth knowing before
spending a day on it.

It is also worth knowing that a proven fallback exists and is closer than it
looks. `recipes/openbcm/recipe.yml` says the SDK's kernel BDE *"use interfaces
removed since — ioremap_nocache went in 5.6"* and does not build them. BISDN
builds them on 6.12, so their fork carries the modernisation. If UIO turns out
to be a dead end here, that is where to look rather than starting over.

**armel against armhf, and this one is settled.** Their machine is
`generic-armel-iproc` — `DEFAULTTUNE = "cortexa9"`, soft-float. NOSaic chose
`armhf`. That is not a hardware constraint on their side: the switch in the lab
runs `/lib/ld-linux-armhf.so.3` today under EdgeNOS, so hard-float is proven on
this silicon. Their soft-float is most likely about the OF-DPA binaries they
link against, which NOSaic does not use — it builds the SDK from source. Keep
`armhf`.

## What to do with this

Nothing in here changes what the board *is*. It changes how much of the kernel
this port has to bring with it, and that is a decision rather than a bug fix:

1. **Try mainline first anyway**, because the boot will say. The failures
   predicted above are specific and cheap to observe — no `/dev/sda`, no
   management interface — and a TFTP boot writes nothing.
2. **If they happen, take the patches.** They are GPL-2.0 kernel patches
   against the same series NOSaic builds, and `recipes/linux/patches/` already
   exists. Take the four that matter (`10-06`, `10-10`, `10-15`, `10-17`)
   rather than all nineteen, since `ARCH_BCM_HR2` already covers what three of
   the others widen.
3. **Do not adopt `ARCH_XGS_IPROC` wholesale without a reason.** A 165 KB
   machine patch for a SoC family mainline already supports is a large thing to
   carry, and the parts of it this board needs are separable.

Their patch set is worth reading in full before the first boot rather than
after: `meta-open-network-linux/recipes-kernel/linux/linux-yocto-onl-linux-6.12.y/bisdn-kmeta/bsp/armel-iproc/`.
