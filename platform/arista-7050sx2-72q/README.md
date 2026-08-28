# Arista DCS-7050SX2-72Q-R

NOSaic's first real board, and the one M6 is written against.

| | |
|---|---|
| ASIC | Broadcom **BCM56860** (Trident2+), `14e4:b860` |
| Arch | x86_64 |
| Front panel | 48 × SFP+ (10G) + 6 × QSFP+ (40G) — 72 × 10G |
| Management | BCM57762 (`14e4:1682`), `tg3` |
| Bootloader | Aboot, unsigned SWIs |
| Console | ttyS0 @ 9600 |
| Profile | full |
| Status | **planned** — nothing here has run on the switch |

- **[Install](docs/install.md)** — getting NOSaic onto the switch
- **[Build](docs/build.md)** — building an image for it
- **[Hardware reference](docs/hardware.md)** — topology, boot chain, port map, quirks

## Why this board first

The plan originally named the 7050TX-64. It is now second. The TX's 48 ports of
10GBASE-T need external PHYs with firmware managed over MDIO, where this board's
SFP+/QSFP+ cages are direct serdes — and the reverse engineering on this box is
much further along, on the path NOSaic actually takes.

## What is not done

Everything that makes it a switch. There is no `nosd-td2p`, no OpenBCM recipe,
no BDE/KNET modules, no platform HAL. The base OS boots and upgrades itself on
x86_64, which is this board's architecture, so that half transfers unchanged.

## Reverse engineering

In a private repository outside this tree. What is here is the board as NOSaic
drives it; the investigation — traces, hypotheses, eliminated leads, and
anything derived from vendor binaries — stays there. Vendor SDK source is never
copied in; it is referenced by `file:line`.
