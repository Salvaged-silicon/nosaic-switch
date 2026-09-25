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
| Status | **bringup** — boots and routes over three 40G and three copper ports; 41 of 48 copper ports never cabled |

- **[Hardware reference](docs/hardware.md)** — diagrams, port map, registers, quirks
- **[Build](docs/build.md)** — building an image for it
- **[Install](docs/install.md)** — console, getting an image on, and back to the vendor OS
- **[Todo](docs/todo.md)** — what is left, and what blocks what

## What works

NOSaic boots this switch, drives the Trident2, and routes over all three of its
cabled 40G links. Measured on the hardware:

- **Boots itself from a cold power cut.** `boot-config` names NOSaic, so nothing
  needs a console or an Aboot prompt. Measured with the PDU outlet pulled and
  the box confirmed dark: ssh at 79 seconds, the datapath at 532, and every port
  and adjacency back with nothing typed. The vendor OS stays on flash and Aboot
  still boots it on demand, which is the way back.
- **A/B upgrade works in both directions, unattended.** A rootfs streamed to the
  switch, installed into the inactive slot by the running CLI, and booted; an
  image the health check declined was left to roll back, and a healthy one
  committed itself.
- **VLAN trunks and SVIs** ([docs/vlan.md](../../docs/vlan.md)). et49 as
  `trunk 100 native 200` to the 7050SX2's Et52, with an SVI in each VLAN at
  both ends: tagged and native both carried traffic, OSPF went Full over the
  native SVI, and the SX2 routed into the tagged VLAN to this box in hardware.
  The first td2 board proven, so the Trident2 datapath does VLANs as the
  Trident2+ one does.
- **The management VRF** ([docs/vrf.md](../../docs/vrf.md)): eth0 in
  table 1001, no pin route, and all five adjacencies unaffected.
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
- **The copper ports route.** All 48 BCM84848s answer `0x600d`, take their
  firmware over the SCD's MDIO bus and bind to Broadcom's driver, and the MAC
  follows what the wire negotiates — which is the whole reason
  [phy.c](../../datapath/td2/phy.c) exists. Seven are cabled and each came up at
  the speed its far end offered: `et1`/`et2` at 1000 on SGMII, `et3`/`et4` at
  10000 on XFI, `et5` at 1000, and `et31`/`et32` at 10000. Three of them carry
  routed traffic to real neighbours — `et31` and `et32` to a Nexus 3172TQ with
  an OSPFv2 adjacency on each, and `et5` to the lab's management switch as an
  out-of-band path. Traffic entering `et5` and leaving a 40G port and back has
  been measured end to end, which is forwarding between two different front
  panel ports rather than a loopback.
- **Copper link LEDs follow link.** Driven through the PHY's own control word
  rather than the board controller — the SCD path drives the QSFP cages only.
  The write is read back (`0x4924` dark, `0x4922` lit) because
  `bcm_port_phy_set` reports success on these parts without reaching them.

**It is still `bringup`, and the reasons are specific.** Forty-one of the 48
copper ports have never been cabled, so what is claimed above is seven ports and
not a panel. The watchdog is not armed, because arming it without a petting
service is a timer that power-cycles the switch. `prefdl` is unread, so the
board cannot say what it is and the management MAC lives in a config file. The
cooling band is carried from the predecessor rather than measured here, and the
fan sits at 100% because the board idles at the top of it. Readdressing a routed
port needs a datapath restart to reach the chip.

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
