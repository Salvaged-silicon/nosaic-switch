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
| Status | **bringup** — NOSaic has not booted on this board yet |

- **[Hardware reference](docs/hardware.md)** — diagrams, port map, registers, quirks
- **[Build](docs/build.md)** — building an image for it
- **[Install](docs/install.md)** — console, getting an image on, and back to the vendor OS
- **[Todo](docs/todo.md)** — what is left, and what blocks what

## What works

**Nothing in NOSaic yet.** This port is `bringup`: the board directory exists,
`board.yml` is filled in from measurements, and the hardware is documented. No
NOSaic image has been built or booted here.

What is established is the *board*, under **EdgeNOS** — the predecessor project,
which boots this switch and forwards in hardware. Everything in
[hardware.md](docs/hardware.md) was read off the running unit rather than
inferred, and that is the value this directory carries today. EdgeNOS reaches
hardware forwarding for IPv4 and IPv6, OSPFv2/v3 adjacencies, ECMP programmed as
a shared group, LEDs, and PSU and thermal monitoring — none of which is NOSaic
code.

Being explicit about that gap is the point of shipping the port at `bringup`
rather than holding it back: somebody with this switch in a rack can see that
the project knows the hardware and where the work stopped.

## What this board needs that the SX2 did not

The 48 copper ports sit behind **BCM84848 PHYs whose firmware loads over the
SCD's MDIO bus**, and nothing in the tree does that yet — both existing boards
use direct-serdes cages. That layer is the work here; the rest has a sibling to
copy, including the SCD platform HAL and the userspace-BDE approach to the chip.

Three of the quirks in [hardware.md](docs/hardware.md) exist only because of
those PHYs, and each cost real time to find: a link with no speed is not a link,
the MAC interface must follow the negotiated speed, and MDIO is a shared bus the
datapath depends on.

## An open question before this goes further

⚠ **The cooling curve is not in `board.yml`, deliberately.** This board's fan
policy was extracted from the board description file on the switch, which is
vendor-confidential, so the numbers are not ours to publish here — even though
the vendor publishes the same class of data for other boards, and even though
the SX2 carries its thermal band inline.

EdgeNOS handles this by reading the curve at runtime from a file the operator
generates on their own switch, the same split this project uses for the port map.
NOSaic's `thermal:` schema expects values in `board.yml` instead, so one of three
things has to happen, and it is a decision rather than an oversight:

1. ship a generator in `tools/` and teach the thermal code to read its output, as the port map already works;
2. establish a curve on this board by our own measurement and commit that;
3. decide the simplified `min_c`/`max_c` band is the public class of data the audit found it to be, and commit it.

Until then the board declares no thermal policy, which means the fans run at the
controller's own default rather than to a curve.

## Reverse engineering

In a private repository outside this tree, one per switch:
`td2-7050tx64-reverse-engineering`. It holds the traces, the register captures,
the theories that died, and everything derived from the vendor's own files — the
SDK configuration capture, the board description file and the values extracted
from it, and the port map and polarity data.

None of that belongs here. `docs/hardware.md` documents the board **as NOSaic
drives it**; the investigation stays where it is, and vendor SDK source is
referenced by `file:line` rather than copied.
