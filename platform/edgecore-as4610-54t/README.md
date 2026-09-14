# Edgecore AS4610-54T

NOSaic's fifth board, its first on ARM, and the first whose CPU is inside the
switch chip rather than attached to it.

**Nothing here has been booted.** The port is written — a new architecture, a
device tree, a datapath and a platform HAL — and every line of it is unproven
on hardware.

What it is not is guesswork. The facts underneath it were read off the switch
rather than copied from the project it replaces: the FIT load addresses out of
the image it boots today, the bootloader's capabilities out of its own flash,
the CPU's feature list out of its ID registers, the device tree out of the
running system. That is the half of a bring-up normally discovered one wrong
guess at a time, and it is still not the same thing as a switch that forwards.

| | Where | State |
|---|---|---|
| Architecture | [`arch/armhf`](../../arch/armhf/arch.yml) | written; never compiled |
| Kernel config | [`recipes/linux/config/armhf.fragment`](../../recipes/linux/config/armhf.fragment) | every symbol checked against mainline 6.12 |
| Device tree | [`dts/as4610-54t.dts`](dts/as4610-54t.dts) | compiles; mainline bindings throughout |
| Datapath | [`datapath/helix4/`](../../datapath/helix4/) | compiles, with and without the SDK |
| Platform HAL | [`internal/platformhal/as4610/`](../../internal/platformhal/as4610/) | compiles; decodes covered by tests |

See [todo.md](docs/todo.md) for what is left, in the order it has to happen.

| | |
|---|---|
| ASIC | Broadcom **BCM56340** (Helix4), on-die CMIC at `0x48000000` — no PCI |
| Arch | **ARMv7-A**, 32-bit little-endian, dual **Cortex-A9** in the chip's iProc block |
| Memory | 768 MB |
| Disk | 29 GB, behind the SoC's USB controller |
| Front panel | 48 × 1000BASE-T + 4 × SFP+ (10G) + 2 × SFP+ stacking — `ge0`..`ge47`, `xe0`..`xe5` |
| Management | The SoC's own AMAC, behind an SGMII PHY |
| Boot | U-Boot 2012.10 → ONIE (in NOR flash) → FIT |
| Console | ttyS0 @ **115200** |
| ONIE platform | `arm-accton-as4610-54-r0` — Accton is the ODM, Edgecore the brand |
| Status | **planned** — not built, not booted |

- **[Hardware reference](docs/hardware.md)** — the block diagram, the boot chain, the port map, and what makes this board different
- **[Build](docs/build.md)** — building an image for it, and what does not exist yet
- **[Install](docs/install.md)** — getting it onto the switch, and getting back
- **[BISDN prior art](docs/bisdn-prior-art.md)** — the other project running mainline on this board: what it confirms, and the two things it says this port has wrong
- **[Todo](docs/todo.md)** — the ordered path from here to a switch that forwards

> **Read the prior art before the first boot.** BISDN Linux runs plain
> `linux-stable` 6.12 on this switch — the same kernel series NOSaic builds —
> and carries nineteen out-of-tree patches to do it. Four of our measured
> numbers are corroborated there, including the second CPU's boot register.
> Two of our assumptions are contradicted, and they are the disk and the
> management port.

## Why this board

Every NOSaic board so far has had the same shape: a CPU, a PCI bus, and a
switch chip on the far end of it. The axes changed — x86 to PowerPC, Trident2+
to Trident+, Aboot to ONIE — and that shape did not. It is baked into the
datapath in a place nobody chose to put it there: **`bde.c` opens a PCI device.**

This board does not have one. The BCM56340's iProc block *is* the host: two
Cortex-A9 cores on the same die as the forwarding pipeline, reaching it through
a CMIC that appears as a platform device at a fixed physical address. There is
no bus to enumerate, no configuration space, no BAR to map, and no bridge whose
bus-mastering bit needs setting — which was the AS5610's hardest bug and is
structurally absent here.

So this is the port that answers whether "the core has no board knowledge" also
means "the datapath has no bus knowledge". It changes three things at once, and
each is meant to be additive:

- **`arch/armhf`** is new, and is written but unbuilt. It is the third
  architecture and the second 32-bit one.
- **`asic: helix4`** is a new family. OpenBCM 6.5.24 carries it —
  `src/soc/esw/helix4.c` and `src/soc/mcm/bcm56340_a0.c` — so the SDK side is
  chip support that already exists rather than something to write.
- **`boot: onie-sfx`** is not new; the AS5610 uses it. What is new is that
  here it is safe to use in full, for the reason in the next section.

It also answers a question the AS5610 could only raise. That board runs the C
CLI because the gc toolchain has never targeted 32-bit big-endian PowerPC, and
the fear was that end-of-service-life hardware would keep landing outside Go's
reach. It does not: `GOARCH=arm` is a first-class target, the CLI cross-builds
to an 8 MiB static binary, and this board's HAL is the first written in Go
rather than in `cli/`.

One thing found while checking that, because it is the shape of hazard this
architecture has. The ARM CLI **contains** a `UDIV` — a hardware divide this
Cortex-A9 does not have — inside Go's `runtime.udiv`, which reads
`internal/cpu.ARM.HasIDIVA` and takes the software path when it is clear. A
correct binary carrying an instruction the CPU cannot execute is normal here,
and it is why [`arch/armhf`](../../arch/armhf/arch.yml)'s instruction audit
speaks only for what the compiler emits unconditionally.

## ONIE is in flash here, and that changes the install

On the AS5610, ONIE and the NOS share one disk, so writing a whole disk image
takes ONIE with it and removes the way back. That board's install page is
shaped around avoiding exactly that.

Not here. `/proc/mtd` on this switch is four NOR partitions —

```
mtd0  uboot       896 KiB
mtd1  shmoo        64 KiB
mtd2  uboot-env    64 KiB
mtd3  onie          7 MiB
```

— and the firmware's `onie_bootcmd` copies ONIE out of flash at `0x1e100000`
and boots it. Nothing about ONIE is on `/dev/sda`. The disk is entirely ours,
the vendor's ONL partitions included, and a botched install is recovered by
choosing ONIE at the boot menu rather than by finding a vendor image.

## What EdgeNOS already learned here

EdgeNOS runs this switch today, and it is the reason several things in this
directory are facts rather than guesses — the CPLD registers that release the
PHYs, the fan and PSU register map, the SDK properties this chip wants.

It is also where the port map came from, and the map has a shape worth knowing
before touching a port number: **three numbering schemes exist on this box and
they are not the same.** Netdev (`ge0`..`ge47`), ASIC logical port (1..49,
50..55), and the number silkscreened on the metal. The first two are related by
arithmetic. The third is not written down anywhere in software, and the two
points that have been established run *backwards* relative to each other. See
[hardware.md](docs/hardware.md#the-port-map).

What does **not** carry over is the punt path. EdgeNOS uses Broadcom's KNET
kernel module: the chip's DMA rings are owned by the kernel, which presents
`geN`/`xeN` netdevs, and the daemon only installs filters. NOSaic has no kernel
module on any board — the datapath daemon owns the rings from userspace and
bridges them to tap devices, which is what `datapath/common/tapbridge.c` is.
Both work. They are not the same design and the EdgeNOS code does not port.

## One thing you have to generate yourself

The 48 copper ports each sit behind an external PHY, and where each of those
answers on MDIO is not published anywhere. It has to be read off a switch
running its vendor's software, which makes it the vendor's data rather than
ours — so this board ships the generator and not its output, the same rule the
Arista boards' port maps follow.

```sh
platform/edgecore-as4610-54t/tools/mkphymap.sh root@myswitch > phymap.conf
mv phymap.conf platform/edgecore-as4610-54t/config/
```

It reads one file and writes nothing, and it works against stock Edgecore
ICOS, ONL, Cumulus or EdgeNOS — anything that keeps a `config.bcm`. The result
is gitignored and ships in every image you build for this board.

Everything that is *not* vendor-derived is already in
[config/asic.conf](config/asic.conf): the port bitmaps
follow from what the switch is sold as and from Helix4's fixed logical port
numbering, and the table sizes and bring-up knobs are chip behaviour. **There
is no SerDes port map to generate here** — that is the useful difference from
the Trident boards, where which logical port reaches which lane is board wiring
and has to be discovered.

## The unit in the rack is not the one to install on

There are two AS4610s. The racked one is the lab's out-of-band switch: the
console server for every other box in this project hangs off `ge25`, and it
holds `10.10.200.1`. Installing anything on it removes console access to
everything else, including the boards this port would be compared against.

Bring up the spare. [install.md](docs/install.md) assumes you have.
