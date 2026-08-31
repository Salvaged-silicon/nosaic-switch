# Arista DCS-7050SX2-72Q-R

NOSaic's first real board, and the one M6 is written against.

| | |
|---|---|
| ASIC | Broadcom **BCM56860** (Trident2+), `14e4:b860` at `01:00.0` |
| Board controller | Arista SCD, `3475:0001` at `05:00.0` |
| Arch | x86_64 |
| Front panel | 48 × SFP+ (10G) + 6 × QSFP+ (40G) — 72 × 10G |
| Management | BCM57762 (`14e4:1682`), `tg3` |
| Bootloader | Aboot, unsigned SWIs |
| Console | ttyS0 @ 9600 |
| Status | **bringup** — boots from its own flash, forwards and routes; no A/B slots or persistence yet |

- **[Architecture](docs/architecture.md)** — how it all works, hardware and software
- **[Running from RAM](docs/running.md)** — the development loop, no flash written
- **[Build](docs/build.md)** — building an image for it
- **[Hardware reference](docs/hardware.md)** — registers, port map, quirks
- **[Install](docs/install.md)** — the flash route (not yet proven here)

## What works

The board runs as a router. On the switch, verified rather than assumed:

- the chip comes out of reset, enumerates and initialises through the SDK;
- ports link, and front-panel ports appear on the Linux stack as `et1`, `et2`;
- FRR holds **OSPFv2 and OSPFv3 adjacencies at Full** with two different
  vendors' boxes;
- the kernel FIB is mirrored into the ASIC — `CHIP route 96/8192`, from the
  chip's own accounting;
- **forwarding happens in silicon**: 100 packets routed through the box raised
  the chip's port counter by 101 and the CPU's by 44, which is the background
  OSPF for four adjacencies and nothing like 100;
- fans, temperatures, PSUs and transceivers read and control through the
  platform HAL, with a closed-loop fan curve;
- `reboot` works;
- **it boots from its own flash, unattended** — Aboot reads `boot-config`,
  finds the SWI and boots it with no console intervention and no network, and
  comes up with its datapath running and its ports present.

## What does not

- **A/B slots and rollback.** What is installed is one image Aboot boots
  directly. The slot machinery is CI-tested on the virtual platform and
  unexercised here.
- **Addressing and routing do not survive a reboot.** The datapath does: the
  port map and polarity ship in the image, so after a cold boot `nosd` is
  running and the ports exist. What is missing is somewhere for *this* switch's
  addresses to live, and an ordering fix — network configuration is applied
  before the datapath has created the interfaces it would apply to.
- **The `full` profile.** `board.yml` says `full` (systemd); only `minimal`
  (s6) has been booted.
- **ECMP.** `l3sync` takes one next hop per prefix.

Status stays `bringup` until it boots from its own flash and survives a power
cycle without help.

## Why this board first

The plan originally named the 7050TX-64. It is now second. The TX's 48 ports of
10GBASE-T need external PHYs with firmware managed over MDIO, where this board's
SFP+/QSFP+ cages are direct serdes — and the reverse engineering on this box was
much further along, on the path NOSaic actually takes.

## Reverse engineering

In a private repository outside this tree. What is here is the board as NOSaic
drives it; the investigation — traces, hypotheses, eliminated leads, and
anything derived from vendor binaries — stays there. Vendor SDK source is never
copied in; it is referenced by `file:line`.

Two files are generated per switch and deliberately absent from this
repository: `portmap.conf` and `polarity.conf`. The numbers in them are the
vendor's, read off the machine that already has them. The generators are in
[tools/](tools/); the output is yours.
