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

- **[Walkthrough](docs/walkthrough.md)** — **start here** if you have one of these:
  backup, port map, build, install, forward
- **[Architecture](docs/architecture.md)** — how it all works, hardware and software
- **[Running from RAM](docs/running.md)** — the development loop, no flash written
- **[Build](docs/build.md)** — building an image for it
- **[Hardware reference](docs/hardware.md)** — registers, port map, quirks
- **[Install](docs/install.md)** — the flash route (not yet proven here)

## What works

The board runs as a router. On the switch, verified rather than assumed:

- the chip comes out of reset, enumerates and initialises through the SDK;
- ports link, and front-panel ports appear on the Linux stack as `et1`, `et2`,
  `et53`, `et54`;
- **both 40G QSFP+ cages link at 40000 and pass traffic**, to two different
  neighbours, from a cold boot with no manual step — see
  [hardware](docs/hardware.md#the-40g-cages) for what that took;
- FRR holds **OSPFv2 and OSPFv3 adjacencies at Full** with two different
  vendors' boxes;
- **its addressing and OSPF configuration come back on their own** after a power
  cycle: loopback, every routed port, and the routing daemons, nothing typed in;
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
- **The control plane moves almost nothing.** Every frame destined for this
  switch is punted through the CPU by the tap bridge, and that path measures
  **28 KB/s — about 19 packets a second** — while answering a ping in 8 ms.
  Enough for the protocols a switch runs; not enough for anything else. A
  full-rate bulk transfer across it wedged the datapath outright, twice,
  recoverable only by a power cycle. Details and the two SDK defaults behind it
  are in [todo](docs/todo.md).
- **No way in over the network.** NOSaic ships no SSH server, so the only shell
  is the serial console at 9600. That is a missing package, not a missing
  capability, but it makes every remote operation slow.
- **The `full` profile.** `board.yml` says `full` (systemd); only `minimal`
  (s6) has been booted.
- **ECMP.** `l3sync` takes one next hop per prefix.

Status stays `bringup` until it boots from its own flash and survives a power
cycle without help.

## Getting the port map and polarity from EOS

Two files are needed before this board can forward anything, and neither is in
this repository:

| File | What it is |
|---|---|
| `portmap.conf` | which logical port is wired to which physical SerDes lane |
| `polarity.conf` | which lanes have their differential pair swapped on the PCB |

They are **board** data — every 7050SX2-72Q has the same lane map and the same
PCB polarity, so you generate them once and they serve every switch of this
model you own. They are absent from this repository because the only practical
way to read them is to ask the vendor's software on a machine that already has
them, which makes the output vendor-derived. The generators are ours; the
numbers are yours.

Neither can be guessed. The map is not an offset — on this board Ethernet1..20
sit on lanes 13..32, Ethernet21..48 on 41..68, and the QSFP cages are not in
order, with Ethernet50 below Ethernet49. A sequential map satisfies every
bandwidth rule the chip enforces and reaches none of the right cages. Polarity
is worse: getting it wrong brings the link **up** and makes every frame
garbage, because inverting a 64b/66b stream turns the sync header `01` into
`10`, which is also legal. The receiver locks onto nonsense and reports no
errors.

### 1. Get to EOS

The generators read from a switch running the vendor's OS. If yours already
boots NOSaic, boot EOS once from the Aboot prompt — this writes nothing and
leaves `boot-config` alone:

```
Aboot# boot flash:/EOS-4.18.3.1F.swi
```

Note EOS's own management address, which is not necessarily the one NOSaic uses.

### 2. Generate them

Both are **read-only**: `mkportmap.sh` issues a `show` command, and
`mkpolarity.sh` issues register reads. Nothing is written to the switch, which
may be carrying traffic while they run.

```sh
cd platform/arista-7050sx2-72q/tools
./mkportmap.sh  <switch-ip> > portmap.conf
./mkpolarity.sh <switch-ip> > polarity.conf
```

Both need `sshpass` and take `SW_USER` (default `admin`) and `SW_PW` (default
`arista`) from the environment. `mkportmap.sh` also takes `SFP_CAGES`, which
defaults to 48 — the number of front-panel cages that are SFP+ rather than
QSFP+.

⚠ `mkpolarity.sh` uses `phy raw sbus`. Do not substitute `getreg`: a blind
`getreg` sweep wedged this box hard enough to need a physical power cycle.

**No management address on the switch?** `mkportmap.sh` will take the command's
output from anywhere, so you can capture it over the serial console instead:

```
switch# show platform trident system detail          (save the output)

./mkportmap.sh --stdin < captured.txt > portmap.conf
```

### 3. Put them where the build will find them

```sh
cp portmap.conf polarity.conf platform/arista-7050sx2-72q/config/
```

`.gitignore` keeps those two filenames untracked, and the image builder copies
a board's `config/` into `/etc/nosaic` — the first place the datapath looks. The
build tells you how many it took:

```
4 board configuration file(s) into /etc/nosaic
```

Four is right for this board: `asic.conf`, `network.conf`, and these two. Two
means the image has no datapath — it will boot, `nosd` will exit, and s6 will
restart it forever with the log naming the file it wanted.

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

`portmap.conf` and `polarity.conf` are absent from this repository for the same
reason — the numbers are read off a machine running the vendor's software. See
[Getting the port map and polarity from EOS](#getting-the-port-map-and-polarity-from-eos).
