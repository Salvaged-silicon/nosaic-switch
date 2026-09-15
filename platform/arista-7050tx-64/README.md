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
| Status | **bringup** — boots, forwards and routes on all three 40G links; copper carries frames, nothing routed over it yet |

- **[Hardware reference](docs/hardware.md)** — diagrams, port map, registers, quirks
- **[Build](docs/build.md)** — building an image for it
- **[Install](docs/install.md)** — console, getting an image on, and back to the vendor OS
- **[Todo](docs/todo.md)** — what is left, and what blocks what

## What works

NOSaic boots this switch, drives the Trident2, and routes over all three of its
cabled 40G links. Measured on the hardware:

- **Boots itself.** `boot-config` names NOSaic, so a reboot needs no console and
  no Aboot prompt: measured at 100 seconds from `reboot` to ssh, into an A/B
  slot with a persistent data image. The vendor OS stays on flash and Aboot
  still boots it on demand, which is the way back.
- **A/B upgrade works in both directions, unattended.** A rootfs streamed to the
  switch, installed into the inactive slot by the running CLI, and booted; an
  image the health check declined was left to roll back, and a healthy one
  committed itself.
- **The chip initialises.** `soc_misc_init`, `soc_mmu_init`, `bcm_attach`,
  `bcm_init` and `bcm_stat_init` all complete, 52 ports are created from the
  generated port map, and the four QSFP cages land on SDK ports 49, 53, 57 and
  61 exactly as [the port map](docs/hardware.md#port-map) says.
- **All three 40G links forward and route.** `nosaic show ports` answers from
  the silicon — `et49 et50 et52`, all `up 40000` at MTU 1600 — with three
  OSPFv2 adjacencies Full, an OSPFv3 adjacency on `et52`, and 23 routes
  programmed into the chip.
- **ECMP is real**, not just configured: `l3: ecmp group of 2 -> egress 200000`,
  a shared `bcm_l3_egress_ecmp` group carrying both members of the pair, and the
  chip reports `ecmp yes, up to 1024 paths`.
- **The board's own hardware is driven** — four thermal sensors and fan control,
  PSU presence, chassis lamps, the QSFP cages, the transceiver EEPROMs, and the
  DS100KR800 signal repeater in front of the last two cages.
- **The copper PHYs link, and the MAC follows what they negotiate.** All 48
  BCM84848s answer `0x600d`, take their firmware over the SCD's MDIO bus and
  bind to Broadcom's driver; four ports have been cabled and all four came up —
  `et1`/`et2` at 1000 with the MAC on SGMII, `et3`/`et4` at 10000 on XFI. The
  MAC-interface matching is the whole reason [phy.c](../../datapath/td2/phy.c)
  exists, and those two pairs are the first evidence it works. Frames cross:
  `et3` and `et4` are patched together and four ARP frames sent each way were
  received each way. ⚠ **The datapath is what is proven, not routing.** Both
  ends of that patch are the same host, so Linux answers no ARP — a real
  neighbour or a network namespace is what the next step needs.

**It is still `bringup`, and the reasons are specific.** The copper ports link
but have carried nothing: four of the 48 have been cabled, none has an address,
and no frame has crossed one in either direction. The watchdog is not armed,
because arming it without a petting service is a timer that power-cycles the
switch. `prefdl` is unread, so the board cannot say what
it is and the management MAC lives in a config file.

Everything left is in [todo](docs/todo.md).

The board itself was established first under **EdgeNOS**, the predecessor
project, which forwards in hardware here. Everything in
[hardware.md](docs/hardware.md) was read off the running unit rather than
inferred.

## What this board needs that the SX2 did not

The 48 copper ports sit behind **BCM84848 PHYs whose firmware loads over the
SCD's MDIO bus**, where both existing boards use direct-serdes cages. Nothing
in the tree did that, and it was the work here: [scdmdio](../../datapath/scdmdio/)
drives the controller's MDIO accelerators, [phybus.c](../../datapath/td2/phybus.c)
hands the SDK a bus it can reach the parts on, and [phy.c](../../datapath/td2/phy.c)
keeps the chip's MAC side agreeing with what the wire negotiated. The rest had a
sibling to copy, including the SCD platform HAL and the userspace-BDE approach
to the chip.

Three of the quirks in [hardware.md](docs/hardware.md) exist only because of
those PHYs, and each cost real time to find: a link with no speed is not a link,
the MAC interface must follow the negotiated speed, and MDIO is a shared bus the
datapath depends on.

## Before you build one

⚠ **Four files must be generated against your own switch**, because they are
this board's vendor data and are not shipped: the port map, the SerDes
polarity, the repeater tuning and the transmit taps. `tools/` has a generator
for each, [build.md](docs/build.md) has the commands, and each one fails
silently in its own way if you skip it.

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
