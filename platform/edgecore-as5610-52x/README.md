# Edgecore AS5610-52X

NOSaic's third board, and the first that is not x86_64.

**It is installed and it forwards.** NOSaic is on this board's own disk, put
there by its ONIE installer, and it is what the switch boots — the vendor OS is
no longer on it. Our own PowerPC toolchain, our kernel, our device tree, a
squashfs under an overlay, and A/B slots with the boot pointer on a journal-less
boot partition.

It came up on 2026-09-02 over TFTP with nothing written to the disk, and was
installed to flash on 2026-09-03. Measured on the running switch rather than
recalled:

```
10 front-panel ports up          52 routes in the kernel
7 OSPFv2 and 3 OSPFv3 adjacencies, with two vendors' routers
booted from slot a, on disk, with no hot-patches
```

This is the first board that is not x86_64, and it is the one that tests the
architecture seam rather than the datapath: **PowerPC e500v2**, big-endian,
soft-float. The Go toolchain has never targeted 32-bit big-endian PowerPC, so
this board runs the C CLI from `cli/` instead of the Go one — the same commands
against the same contract, which is checked by diffing the two implementations'
output on a board that can run both.

Since then its datapath has grown to switchapi 1.13 with the other boards',
and these are proven here with traffic against the other NOSaic switches: VLANs
and SVIs, LAG (static and LACP), rapid spanning tree with BPDU guard, MLAG as
a peer of the 7050SX2, the virtual gateway over IPv4, QinQ as provider and
customer, IS-IS, MAC aging, ACLs, and `nosaic verify`. Each has a section
below. The IPv6 virtual gateway is built into its datapath and has not been
run here, and root guard has only been run from the SX2's side of this pair.
OSPF holds 4 neighbours.

See [install.md](docs/install.md) for how to install it and
[todo.md](docs/todo.md) for what is left.

| | |
|---|---|
| ASIC | Broadcom **BCM56846** (Trident+), `14e4:b846` at `0001:01:00.0` |
| Arch | **PowerPC e500v2**, 32-bit big-endian, dual core (Freescale P2020) |
| Memory | 2 GB |
| Front panel | 48 × SFP+ (10G) + 4 × QSFP+ (40G) — `xe0`..`xe47` and `xe48`..`xe51` |
| Boot | U-Boot + ONIE, on NOR flash |
| Console | ttyS0 @ **115200** |
| Device tree | `powerpc-accton-as5610-52x-r0` — Accton is the ODM, Edgecore the brand |
| Status | **experimental** — installed on its own disk, forwards and routes, survives a cold power cut |

- **[Hardware reference](docs/hardware.md)** — what has been read off a running unit
- **[Build](docs/build.md)** — what exists and what is missing
- **[Install](docs/install.md)** — the ONIE route, and how to boot it over TFTP
- **[Configuration](docs/configuration.md)** — what this switch is set up to do,
  captured from the EdgeNOS installation it has to replace

## Why this board

It is the first port that exercises the axes independently rather than
inheriting them. The 7050SX2 and virt-x86_64 are both x86_64, so every claim
about NOSaic being architecture-neutral has been a design commitment rather
than a demonstrated fact. This board changes three things at once — a new CPU
architecture, a new ASIC family, a new bootloader — and each is meant to be a
separate, additive piece of work.

Two of the three already exist in the tree:

- **`arch/powerpc`** is fully specified, and was written with this board in
  mind: `powerpc-nosaic-linux-gnu`, soft-float, big-endian, with a
  `forbidden_insn_re` that fails a build containing any instruction an e500v2
  cannot execute. That audit exists because a hard-float build disassembles
  cleanly and runs under QEMU's permissive generic-PowerPC model, and only
  dies on the real hardware.
- **`internal/boot/onie.go` and `uboot.go`** exist as boot backends.

## What is missing

- ~~**The toolchain has never been built.**~~ Spike S1 passed: gcc-15.2.0 and
  glibc-2.42 build soft-float generic PowerPC, and the instruction audit
  reports `0 forbidden` — nothing in the output uses an opcode an e500v2
  cannot execute. This class of hardware does **not** need a pinned older
  compiler, and M8's scope is unchanged.
- **The Trident+ datapath exists and drives the chip.** `nosd-tdp` is built from
  the same OpenBCM 6.5.24 recipe the 7050SX2 uses, and the per-ASIC split held:
  the two daemons share `datapath/common` — the tap bridge, the route sync and
  the query server — and differ only where the silicon does. It serves the same
  northbound contract, so `nosaic show ports` means the same thing on both.
- **NOSaic is installed on it and EdgeNOS is gone.** Every fact on these pages is
  now read off NOSaic running on the box rather than off the OS it replaced.

## Stopping nosd

Stop it through the supervisor, and pass a timeout:

    s6-rc -t 30000 -d change nosd

Without `-t`, s6 computes a deadline from an infinite relative time and
overflows this board's 32-bit `time_t`:

    s6-svlisten: fatal: unable to subscribe to events ...: Value too large
    s6-rc: warning: unable to stop service ospfd: command exited 111

Same root cause as the `s6-rc-init` overflow the image build already works
around with `-t`; it reaches anyone typing `s6-rc` by hand. A graceful stop and
start is safe -- the chip comes back to 52 ports and its links.

`kill -9` is not. It leaves the chip mid-operation, and the next attach comes
back with TXPLL lock failures, 48 of 52 ports and no link at all, while
reporting that it is serving:

    WC40 : TXPLL did not lock: u=0 p=29
    soc_reg32_read: invalid S-Channel reply, expected READ_REG_ACK
    bcm_init failed in port

Only a board reset clears it, so this is about how the chip is left rather than
the init sequence. The daemon should reset the chip on the way in and out; until
it does, do not `kill -9` a datapath daemon on this board.

## Per-port service VLANs are the resting state

`nosd` puts every front-panel port in its own VLAN, 3300+port, with the port
untagged and the CPU tagged, and empties VLAN 1. That is Cumulus's layout on
this board, by way of EdgeNOS's `vlan_init_resv_per_port()`, and it is what both
of them boot into -- so it is not a guess, it is what the two pieces of software
known to work on this hardware actually do.

The point is that a port is forwarding and fully usable while being alone in its
VLAN, so there is nothing to bridge to and no loop to form whatever the cabling
looks like. Bridging becomes something you ask for -- and since 2026-09-24 you
can: `nosaic switchport` takes a port out of its service VLAN and into user
VLANs ([VLANs and SVIs](#vlans-and-svis) below). Every 3300+port VID stays
reserved, tapped or not, so `vlan add` refuses them.

Emptying VLAN 1 is not tidiness. EdgeNOS's note is specific: leaving it
populated lets the chip's L2 forwarding pick the wrong egress when the CPU
injects a tagged frame on a service VID, and they watched it happen.

`tdp-probe --ports` and `--stats` instead bridge every port together in VLAN 1.
That is what demonstrates the chip moving frames between ports, and it is a loop
wherever two ports reach the same neighbour -- which this board has, with swp1
and swp2 going to the same upstream. The tool says so every time it does it.

## Punt latency

Pinging the Nexus, which answers ICMP in hardware, from this switch:

    min 0.699 ms   avg 1.66 ms   max 2.689 ms

That is the punt path: tap, chip transmit, wire, and back through the receive
path to the CPU.

**Measure against a hardware responder.** Every earlier figure here was taken
against the Arista 7050SX2 on swp8, which is another NOSaic box with a punt path
of its own, and that neighbour -- not this board -- was most of the number. It
read as median 27 ms with a tail past a second, and the same box pinging the
Nexus at the same moment was answering in about one millisecond. A slow reply
from a switch that punts is not evidence about the switch doing the pinging.

The neighbour turned out to be broken in a way that had nothing to do with
either datapath: nosd-td2p passed every SDK message to its log, 176 KB/s, and
that board RAM-boots, so the log had filled all 1.9 GB of its tmpfs. With the
filter it already needed, its log stops growing and it answers in 1.3 ms.

Two real problems did come out of chasing it, and both are fixed. The periodic
work ran on the packet thread, and `nosaic_tap_pump` calls its tick on every
poll wakeup rather than on a timer, so the FIB mirror ran once per received
frame and `bcm_stat_sync()` across all 52 ports every thirtieth frame; that is
now a thread of its own. And the poll interval could not go below 20 ms without
spinning a core, because the SDK busy-waits for short sleeps; `sdk.c` wraps
`sal_usleep` so it can.

Transit does not use this path at all -- it is forwarded in hardware and never
reaches the CPU.

## Environmentals

`nosaic platform status` -- the same command as on any other board:

    board      AS5610-52X, cpld 1.0
    fans       32% (floor 32%)
    psu1       fitted, ok (reg 0x02)
    psu2       fitted, no-power (reg 0x00)
    leds       sys 0x6f  locator 0x01
    temp       temp2    34C (crit 110C)

`nosaic platform thermal` runs the cooling loop as a supervised service. The
board powers up with its fan duty at 31 of 31 and stays there; this walks it to
the floor and holds, and leaves the fans at full on the way out, because nothing
regulates the box after the loop stops.

**This CLI is C, not Go.** The gc toolchain has ppc64 and ppc64le and has never
had 32-bit big-endian PowerPC, so the Go CLI cannot be built for this board at
all -- see cli/. Under `platform` it provides `status`, `thermal`, `ledwalk`,
`transceivers` and `xcvr`, and refuses the rest of the Go CLI's platform
commands by name, so an operator can tell "this board cannot" from "this build
has not". Everything that talks to the datapath is there, down to the newest
commands: `mac aging` / `show mac`, `switchport ... tunnel` and `tpid`, the
`stp port` guards, `mlag ... reload-delay` and `verify ports|routes`
([docs/cli.md](../../docs/cli.md) has the table).

**The PSU decode is not what the register map suggests.** PSU1's status is in
0x02 and PSU2's in 0x01 -- two registers, not two fields of one -- and presence
is *active low*: bit 0 clear means fitted. Read the obvious way it reports no
power supply on a running switch, which is how EdgeNOS's own kernel driver has
it; its Python layer overrides that and notes the map came from Cumulus.
"fitted, no-power" is the normal state for a second supply with nothing plugged
into it.

LEDs are reported raw and not settable. The two registers are known; what their
bits mean is not.

## ECMP

When this was measured, swp1 and swp2 both faced the Nexus at equal OSPF cost,
so routes behind it had two paths. (Since 2026-09-26 they go to the 7050SX2's
et3 and et4; see [Cabling](#cabling).) 150 packets across 30 destination
addresses, forwarded through the
box in hardware, came out 70 on swp1 and 80 on swp2.

Two things had to be right and only one is obvious. `l3sync` reads routes over
netlink, because `/proc/net/route` lists one gateway per prefix and would have
programmed half of every ECMP route while reporting success. And the multipath
hash has to be told what to look at: with a group correctly built and no hash
inputs the chip picks the same member every time -- the same test went 100/0
before `bcmSwitchHashControl` was set.

**Test it with transit traffic, never from the box itself.** Traffic the switch
originates leaves through the tap and is load-balanced by the *kernel*, so it
spreads across both links whether or not the chip does anything at all. That
reads as a pass and proves nothing; it is how the missing hash went unnoticed
here for a commit.

## ACLs

    nosaic acl add 10 deny in swp6 proto icmp src 10.101.101.26/32
    nosaic show acl

Ingress access lists, IPv4 and IPv6, by port, protocol, addresses and L4
ports, permit or deny, each with a hit counter, applied while the switch runs.
What they are and how to read them is [docs/acl.md](../../docs/acl.md); this
is what the board did on 2026-09-16 and 17, with its OSPF adjacencies up
throughout.

A deny of ICMP from the swp6 neighbour took 10 of 10 echo replies and counted
10. A permit of the same at a lower sequence let 10 of 10 through, counted on
the permit and not the deny. One counting permit per neighbour counted only its
own port's hellos, 13, 9 and 10 in 45 seconds. A deny of OSPF on swp6 alone
took that adjacency down and left swp51 and swp52 Full, and unsetting it
brought swp6 back in 32 seconds. Three malformed rules showed their reasons in
`show acl` while the good ones installed around them. The next day, IPv6: a
deny on ICMPv6 from the swp6 neighbour lost 10 of 10 pings while IPv4 pings
kept working, a deny of OSPFv3 on swp6 took that adjacency down and left the
OSPFv2 one on the same port Full, and in both families a rule on TCP source
port 22 counted the neighbour's replies while one on port 23 counted nothing.
The chip holds 1280 IPv4 and 768 IPv6 rules.

Two things had to be found out on the way and both are recorded in the
[todo](docs/todo.md#fixed-on-2026-09-16). The field processor that EdgeNOS
could never arm fires under the SDK's own bring-up; the first test was the
control plane's existing punt rule counting exactly the twenty replies sent
through it. And the SDK's ingress-port bitmap reaches only one of the chip's
two pipelines on this board, so a rule scoped to one port matched every other
until the port went into the key instead.

## VLANs and SVIs

    nosaic vlan add 100
    nosaic switchport swp51 trunk 100 native 200
    nosaic svi add 100

Through the C CLI, the same words as everywhere else
([docs/vlan.md](../../docs/vlan.md)). On 2026-09-24, swp51 was a trunk to the
7050SX2's Et54 with an SVI in each VLAN at both ends:
- tagged 100 and native 200 both passed traffic, and OSPF ran over the native
  SVI;
- the TX's traffic arriving on the routed swp52 was routed by this chip into
  the tagged VLAN, out of swp51, 20/20, with the CPU counters showing only
  background traffic.

That last test found that this chip's L2 table reports swp51 as (module 1,
port 19). The Trident+ spans two module ids, so l3sync's first egress for the
SVI neighbour went out of a dark 10G port. `nosaic_l2_port()` translates it
now.

Two more things are specific to this board:
- **Addresses.** The tap and SVI MACs are derived at boot from eth0's MAC in
  network.conf, `80:a2:35:81:ca:ae`, so they are `02:35:81:ca:ae:xx`, where
  they used to be the `02:00:00:00:00:xx` every NOSaic switch shared.
- **The management VRF** ([docs/vrf.md](../../docs/vrf.md)) runs here. It
  needed the FIT rewritten, since the kernel is outside the A/B slot, and that
  boot found the initramfs was not waiting for the USB disk.

## LAG and spanning tree

    nosaic lag po5 lacp swp1,swp2
    nosaic stp on
    nosaic stp port swp1 bpdu-guard

[docs/lag.md](../../docs/lag.md) and [docs/stp.md](../../docs/stp.md) have
the measurements; in short, against the 7050SX2 on 2026-09-25 and 26:
- **LAG.** LACP on swp51 alone, then po5 over swp1 and swp2 in every mode,
  static and LACP, routed and as a VLAN trunk with SVIs. A member shut under
  four flows cost the two hashed to it 4 of 300 pings, about 200 ms.
- **Rapid spanning tree.** A loop of swp1/swp2 against et3/et4 broken with
  swp2 discarding. The root port lost and back cost 2 pings, the root moved
  cost 1, and po5 as a tree port taking over from swp51 and handing back cost
  5 of 400.
- **BPDU guard,** on 2026-09-27, on swp1, the root port at the time. The
  first BPDU from the SX2 blocked it and the root port moved to swp2, 200 of
  200 pings through. Link down released it and link up tripped it again;
  configured again without the guard, it went back to root and forwarding.
  The C CLI's `show stp` has the GUARD column.

## MLAG

    nosaic mlag on peer-link <port|poN> [reload-delay <s>]
    nosaic lag <poN> lacp <port> mlag <id>

Proven on 2026-09-27 as **a peer of the 7050SX2**, with swp1/swp2 to et3/et4
as the peer-link, the 7050TX-64 dual-homed on one link to each, and a
[virtual gateway](../../docs/gateway.md) `10.99.40.254` on both. Under 20
pings a second from the TX to the SX2 and through the gateway, with no
duplicates in any case:

| Event | Lost |
|---|---|
| The SX2's half down and up | 3 and 17 of 600 |
| This switch's half down and up | 0 of 600 |
| Peer-link lost | 0 of 600 |
| This switch's datapath killed | 0 of 600 |

It rejoined 3 s after its configuration was back.
[docs/mlag.md](../../docs/mlag.md#a-trident-peer) has the test.

⚠ **Found and fixed: Trident+ drops a static station move.** MAC sync installs
the dual-homed device's MACs as static entries on this switch's half, so a
frame from the device that came round through the peer arrives on the
peer-link with a source held static elsewhere. The chip drops that by default,
and at first every frame the TX sent by way of the SX2 died here -- the AS5610
could not even resolve the TX. The Trident2 pair never showed it. The
peer-link's ports now set `bcmPortControlForwardStaticL2MovePkt`.

The IPv6 virtual gateway (switchapi 1.11) is built into this datapath and has
not been run on this board.

## QinQ

    nosaic switchport swp1 trunk <svid> tpid 0x88a8
    nosaic switchport swp52 tunnel <svid>

Proven on 2026-09-28 ([docs/vlan.md](../../docs/vlan.md)) both ways round:
- **Customer:** a plain trunk, tagged 10 and 20 and native 30, across the
  SX2 and TX as the provider.
- **Provider:** paired with the SX2 over swp1-et3 at 0x88a8, swp52 a tunnel
  port, the TX and the Nexus as customers: 20 of 20 on VLANs 10, 20 and
  native 30. A TPID mismatch on the trunk got 0 of 10 through; a match at
  0x9100 or 0x88a8 carried it.

⚠ **Found and fixed:** a port set back to routed kept its provider TPID, and
its OSPF adjacency stayed down. Going back to routed now resets it, re-tested
here.

## IS-IS

FRR's `isisd` runs beside ospfd and ospf6d and is configured in frr.conf
([docs/isis.md](../../docs/isis.md)). On 2026-09-28 it formed a level-2
point-to-point adjacency with the 7050SX2, swp1 to et3, and a prefix only
IS-IS knew, on this switch's loopback, was learned by the SX2 and two hops
away by the TX, and was in both their chips.

⚠ **Found and fixed on the way: this switch's IPv6 loopback was unreachable.**
l3sync never delivered an IPv6 loopback address to the CPU. It does now: in
the [IPv6 gateway](../../docs/gateway.md) test on the SX2 and TX, 20 of 20
pings reached it in transit through the chip.

## MAC aging and verify

    nosaic mac aging 300
    nosaic show mac
    nosaic verify ports
    nosaic verify routes

Learned MACs age out after 300 s by default, 0 for never, and `mac aging <s>`
goes in network.conf too. Before switchapi 1.12 L2 aging was never turned on
and every learned MAC was permanent. `verify ports` and `verify routes` come
back clean here, and name LAG members and switched ports for what they are
rather than flagging them.

## Cabling

As of 2026-09-28:

| Port | Far end |
|---|---|
| swp1, swp2 | 7050SX2 et3, et4 |
| swp51 | 7050SX2 et54 |
| swp52 | 7050TX-64 et52 |
| swp50 | Nexus 3172TQ eth1_53 -- **not a live link**: this board has no swp50 tap |

## What is left before this replaces EdgeNOS

Measured against `edgenos/platform/accton-as5610-52x`.

**Working, on hardware.** Chip init through the SDK; all 52 ports with per-port
service VLANs; CPU punt on taps; hardware L3 with routes in DEFIP; ECMP across
swp1 and swp2 with traffic on both; OSPFv2 with four adjacencies and OSPFv3 with
one; forwarding enabled; cooling and environmentals through `nosaic platform`;
an unattended boot to all of it, 1.7 ms punt latency to a hardware responder,
and ingress access lists, IPv4 and IPv6, that drop in silicon and count what
they matched. Past parity since: VLANs and SVIs, LAG, rapid spanning tree
with BPDU guard, MLAG and the IPv4 virtual gateway, QinQ, IS-IS, MAC aging,
and a datapath restart the addresses survive -- the `network-reconcile`
service puts them back.

**Small gaps.** LED writes: both registers are known and their bits are not.
Per-tray fan status: the register is read and reported raw, because EdgeNOS does
not decode it either and there is no known-good map to copy.

**ACLs are ahead of parity.** EdgeNOS never got them working -- its own notes
record the IFP-arming wall as open -- and NOSaic has them: see [ACLs](#acls)
above. The wall was the bring-up underneath, not the silicon.
