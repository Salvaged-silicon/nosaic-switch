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
| Status | **experimental** — installed on its own flash with A/B slots, survives a cold power cut, forwards and routes |

- **[Walkthrough](docs/walkthrough.md)** — **start here** if you have one of these:
  backup, port map, build, install, forward
- **[Architecture](docs/architecture.md)** — how it all works, hardware and software
- **[Running from RAM](docs/running.md)** — the development loop, no flash written
- **[Build](docs/build.md)** — building an image for it
- **[Hardware reference](docs/hardware.md)** — registers, port map, quirks
- **[Capabilities](docs/capabilities.md)** — what the silicon can do against what NOSaic uses
- **[Install](docs/install.md)** — the flash route: A/B slots, trial boots and rollback

## What works

The board runs as a router. On the switch, verified rather than assumed:

- the chip comes out of reset, enumerates and initialises through the SDK;
- ports link, and front-panel ports appear on the Linux stack as `et1` to
  `et54`;
- **the 40G QSFP+ cages link at 40000 and pass traffic**, to three different
  neighbours, from a cold boot with no manual step — see
  [hardware](docs/hardware.md#the-40g-cages) for what that took;
- **cabled today:** et3/et4 to the AS5610's swp1/swp2, et52/et53 to the
  7050TX-64's et49/et50, et49 to the Nexus 3172TQ's eth1_54, and et54 to the
  AS5610's swp51. FRR holds **six OSPF neighbours** over them. OSPFv3 has run
  here too;
- **its addressing and OSPF configuration come back on their own** after a power
  cycle: loopback, every routed port, and the routing daemons, nothing typed in;
- the kernel FIB is mirrored into the ASIC — `CHIP route 96/8192`, from the
  chip's own accounting — including **ECMP**, proven 2026-09-11: 75 transit
  packets split 40 and 35 across et52 and et53;
- **forwarding happens in silicon**: 100 packets routed through the box raised
  the chip's port counter by 101 and the CPU's by 44, which is the background
  OSPF for four adjacencies and nothing like 100;
- fans, temperatures, PSUs and transceivers read and control through the
  platform HAL, with a closed-loop fan curve;
- `reboot` works;
- **it boots from its own flash, unattended** — Aboot reads `boot-config`,
  finds the SWI and boots it with no console intervention and no network, and
  comes up with its datapath running and its ports present;
- **A/B upgrades, both directions.** The slots are files on Aboot's own
  filesystem, loop-mounted. A healthy image installed into the inactive slot
  boots on trial, judges itself against its own datapath and commits; one built
  with an empty port map — it boots, answers ssh and does not forward — burns
  all three attempts and the switch returns to the slot it was on, unattended.
- **VLANs and SVIs, in the chip** ([docs/vlan.md](../../docs/vlan.md)).
  Et52 and Et54 as access ports in one VLAN, with both neighbours' /29s on its
  SVI: OSPF went Full with both over the SVI, 20/20 pings switched between the
  two neighbours, and 20/20 were routed in from Et53 and out through the SVI.
  The CPU counters were flat for both, so the chip did it. As a trunk to the
  7050TX-64 and to the AS5610, tagged 100 plus native 200, both VLANs carried
  traffic and OSPF ran over the native SVI;
- **LAG, static and LACP** ([docs/lag.md](../../docs/lag.md)), 2026-09-25:
  et52/et53 as a bundle to the 7050TX-64 in every mode, and a LAG to the
  AS5610, first one member on et54 and then two on et3/et4;
- **rapid spanning tree** ([docs/stp.md](../../docs/stp.md)), 2026-09-26, with
  the AS5610: a loop broken, the root port lost and taken over, the root moved,
  a LAG as a tree port, and transit in the chip. `show stp` shows the root
  times, the ones a bridge below the root runs on;
- **root guard** ([docs/stp.md](../../docs/stp.md)), 2026-09-27: on et3, with
  the AS5610 given priority 0, et3 was held and et4 became the root port with
  traffic carrying on; with the priority restored, et3 went back to forwarding
  by itself. BPDU guard was proven on the AS5610's end of the same links;
- **MLAG, as a peer, twice** ([docs/mlag.md](../../docs/mlag.md)). With the
  7050TX-64 on 2026-09-26, the Nexus 3172TQ dual-homed: 0 of 1200 lost with the
  TX's datapath killed. With the AS5610 on 2026-09-27, the TX dual-homed: 3 and
  17 of 600 lost with this switch's half taken down and up, none with the
  AS5610's, and no duplicates;
- **the virtual gateway, IPv4 and IPv6** ([docs/gateway.md](../../docs/gateway.md)),
  2026-09-26 and 27, with the TX as the other peer: with the TX's datapath
  killed, this switch carried 1200 of 1200 pings through the gateway in each
  family, with no duplicates;
- **QinQ, as the provider** ([docs/vlan.md](../../docs/vlan.md)), 2026-09-28:
  S-VLAN 500 at 0x88a8 over et52 to the TX, and again over et3 to the AS5610,
  with customer tagged and native VLANs carried across;
- **IS-IS** ([docs/isis.md](../../docs/isis.md)), 2026-09-28: adjacencies on
  et3 with the AS5610 and on et52 with the TX, IS-IS routes in the chip and
  forwarded, with OSPF left running;
- **ACLs in the chip** ([docs/acl.md](../../docs/acl.md)), 2026-09-17: a deny
  on et52 dropped 5 of 5 pings and counted 5, with every OSPF adjacency Full
  throughout;
- **MAC aging** (`mac aging <s>`, `show mac`): learned MACs now age out, 300 s
  by default; before this they were permanent;
- **`nosaic verify ports` and `verify routes` are clean**, with LAG members and
  switched ports recognised rather than flagged;
- **a datapath restart takes about 50 s**;
- **the management VRF** ([docs/vrf.md](../../docs/vrf.md)): eth0 is in
  table 1001, the pin route is gone, and transfers run at the pinned rate;
- **its addresses are its own**: tap and SVI MACs are derived at boot from
  eth0's MAC in this switch's network.conf. The `tap_mac_base` on flash is now
  redundant, because it matches.

## Worth knowing

- **The control plane is no longer the bottleneck it was.** Frames destined
  for this switch are punted through the CPU; that path carried about twenty
  packets a second and now carries **500/s at zero loss**, with a bulk
  transfer across it leaving the box responsive. The fix was ours, not the
  chip's: the daemon rescanned the whole kernel FIB once per received packet.
  It also takes interrupts from the chip now rather than polling, which
  recovered a core (idle load 2.07 to 0.20) and, on its own, changed the
  packet rate not at all.
- **SSH is key-only, and lands on root.** dropbear refuses any account with a
  blank password before it looks at a key, and the login account has one so the
  console can reach it without a password. Root's is locked, so keys are the
  only way in and a password can never work.

## What does not

- **The `full` profile.** `board.yml` declares `minimal` (s6), the only
  profile that has run here; `full` (systemd) has not been made to boot.
- **The `prefdl` SEEPROM is not read**, so the management MAC comes from
  `config/network.conf` and that file is correct for exactly one switch.
- **`fanread` returns garbage.** Temperatures, PSU presence and fan control all
  read correctly; that one call does not.

It cleared `bringup` on 2026-09-04: installed to its own flash, back in 68
seconds from a cold power cut at the PDU, with the ext4 journal replaying clean
and four OSPF adjacencies re-formed. A healthy image installed into the inactive
slot confirms itself and commits; one that boots and does not forward burns its
three attempts and rolls back, unattended.

It stays `experimental` rather than being called production for the reasons
listed above — `prefdl`, `fanread` — and because nothing here is called
production until it has run somewhere that matters for longer than a lab
afternoon.

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
