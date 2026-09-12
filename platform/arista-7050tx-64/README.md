# Arista DCS-7050TX-64

The copper board. 48 ports of 10GBASE-T behind external PHYs, which is why it is
second in the tree rather than first — see
[the SX2's README](../arista-7050sx2-72q/README.md) for that decision.

| | |
|---|---|
| ASIC | Broadcom **BCM56855** (Trident2), `14e4:b855` at `01:00.0` |
| Board controller | Arista SCD, `3475:0001` at `02:00.0` |
| CPU / arch | AMD GX-420CA, x86_64, 3839 MB |
| Front panel | 48 × 10GBASE-T (BCM84848 PHYs) + 4 × QSFP+ (40G) |
| Management | RJ45, `tg3` |
| Bootloader | Aboot 4.0.7, unsigned SWIs |
| Console | ttyS0 @ 9600 |
| Status | **bringup** — boots, forwards and routes; one of three links still dark |

- **[Hardware reference](docs/hardware.md)** — diagrams, port map, registers, quirks
- **[Build](docs/build.md)** — building an image for it
- **[Install](docs/install.md)** — console, getting an image on, and back to the vendor OS
- **[Todo](docs/todo.md)** — what is left, and what blocks what

## What works

NOSaic boots on this switch, drives the Trident2, and brings its three cabled
40G links up. Measured on the hardware, 2026-09-11/12:

- **Boots from Aboot into its own userland**, into an A/B slot with a persistent
  data image. The management port answers on its configured address.
- **A/B upgrade works in both directions, unattended.** A rootfs was streamed to
  the switch, installed into the inactive slot by the running CLI, and booted;
  an image the health check declined was left to roll back, and a healthy one
  committed itself — `NOSAIC-TRIAL COMMIT slot b is healthy and is now the slot
  this switch boots`.
- **The chip initialises.** `soc_misc_init`, `soc_mmu_init`, `bcm_attach`,
  `bcm_init` and `bcm_stat_init` all complete, 52 ports are created from the
  generated port map, and the four QSFP cages land on SDK ports 49, 53, 57 and
  61 exactly as [the port map](docs/hardware.md#port-map) says they should.
- **Three 40G links are up**, and `nosaic show ports` answers from the silicon:
  `et49 et50 et52`, all `up 40000` at MTU 1600.
- **Thermal control works** — four sensors, and the fans take their commands.
- **The copper PHY layer finds its 48 ports** and is watching them.

- **It forwards, and it routes.** Two OSPFv2 adjacencies with the 7050SX2 over
  both members of the ECMP pair, eleven routes learned with two next hops each,
  and eleven programmed into the chip. That board's loopback answers in 0.4 ms
  across the fabric rather than over the management port.

**What does not work:** the third link, to the Edgecore AS5610, receives
nothing — and the far end's state has not been established. There is no OSPFv3
adjacency, which looks like the neighbour rather than this board. See
[todo](docs/todo.md).

The board itself was established first under **EdgeNOS**, the predecessor
project, which forwards in hardware here — IPv4 and IPv6, OSPFv2/v3, ECMP as a
shared group, LEDs, PSU and thermal monitoring. Everything in
[hardware.md](docs/hardware.md) was read off the running unit rather than
inferred.

## What this board needs that the SX2 did not

The 48 copper ports sit behind **BCM84848 PHYs whose firmware loads over the
SCD's MDIO bus**, and nothing in the tree does that yet — both existing boards
use direct-serdes cages. That layer is the work here; the rest has a sibling to
copy, including the SCD platform HAL and the userspace-BDE approach to the chip.

Three of the quirks in [hardware.md](docs/hardware.md) exist only because of
those PHYs, and each cost real time to find: a link with no speed is not a link,
the MAC interface must follow the negotiated speed, and MDIO is a shared bus the
datapath depends on.

## Cooling

The band is 25–40 °C, narrower and earlier than the SX2's 35–65. That is not a
copy with the numbers changed: 48 copper PHYs dissipate considerably more than
48 SFP+ cages, and this board starts ramping ten degrees sooner.

The two ends are what carries into NOSaic's model. EdgeNOS drives this board in
five discrete steps — 45, 60, 75, 90 and 100 percent, with thresholds at 25,
31.66, 36.66 and 40 °C — and NOSaic expresses a band instead, so the floor below
which spinning up gains nothing and the ceiling above which there is nothing
left to give are the parts that survive the translation.

⚠ The curve has not been validated against this board *under NOSaic*, only
carried from the predecessor that runs it. `fanread` returns garbage on the
sibling board; whether it is trustworthy here is unestablished.

## Reverse engineering

In a private repository outside this tree, one per switch:
`td2-7050tx64-reverse-engineering`. It holds the traces, the register captures,
the theories that died, and everything derived from the vendor's own files — the
SDK configuration capture, the board description file and the values extracted
from it, and the port map and polarity data.

None of that belongs here. `docs/hardware.md` documents the board **as NOSaic
drives it**; the investigation stays where it is, and vendor SDK source is
referenced by `file:line` rather than copied.
