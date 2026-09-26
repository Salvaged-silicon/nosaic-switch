# MLAG

Multi-chassis link aggregation, the classic peer-link kind: two NOSaic
switches, joined by a peer-link, each have a LAG with the same MLAG id. A
device cabled to both sees one LACP partner on every link and bundles them
as one LAG. Either switch can fail, or its link to the device can, and the
device carries on over the other. This is what Arista calls MLAG and Cisco
calls vPC. switchapi 1.5.

Proven on 2026-09-26 with traffic:

- **Peers:** an Arista 7050SX2-72Q (Trident2+, td2p) and an Arista 7050TX-64
  (Trident2, td2).
- **Peer-link:** their two 40G links as one LAG.
- **Dual-homed:** a Cisco Nexus 3172TQ, with one link to each peer in one
  LACP LAG.
- **Orphan:** an Edgecore AS5610-52X on one link to the SX2 alone.

The AS5610's datapath (tdp) implements MLAG too, but has not been one of a
pair. The virtual board refuses MLAG: two Linux bonds cannot present one LACP
system.

## Commands

    nosaic mlag on peer-link <port|poN> [peer-address <ip>] [priority <n>]
    nosaic mlag off
    nosaic lag <poN> lacp|static <port,...> mlag <id>
    nosaic show mlag

- **The peer-link** is a port or a LAG between the two switches. It has to
  carry every VLAN an MLAG interface is in: make it a trunk with
  `switchport`.
- **The peer address** is the other switch's management address. The
  heartbeat to it runs in the management VRF, and is what tells a dead peer
  from a dead peer-link. Without it, a peer-link lost is taken as the peer
  lost.
- **Priority** elects the primary: lower wins, and the lower MAC breaks a tie.
  The default is 32768.
- **An MLAG interface** is a LAG with an MLAG id from 1 to 1000. Give the two
  halves the same id.

One side of the lab pair, the 7050SX2:

    nosaic lag po1 lacp et52,et53
    nosaic lag po7 lacp et49 mlag 7
    nosaic mlag on peer-link po1 peer-address 10.10.34.3 priority 100
    nosaic vlan add 400
    nosaic switchport po1 trunk 400
    nosaic switchport po7 access 400

The 7050TX-64 had the same lines with its own ports, et49,et50 and et31, and
the SX2's management address. The Nexus had nothing MLAG about it, only a LAG:

    nosaic lag po7 lacp eth1_54,eth1_31

`show mlag`:

    role          primary
    peer          0273dafe7a50
    peer-link     po1, link up, peer heard
    heartbeat     heard
    lacp system   02a8eb93f650
    synced macs   2

    ID  LAG  LOCAL  PEER  STATE
    7   po7  up     up    active

An MLAG interface's state is one of:
- `active`: both halves up;
- `local` or `peer`: one half up;
- `down`;
- `disabled`: a secondary that has shut its half, because the peer-link is
  gone and the peer is not.

In network.conf the same lines go in as they are. `mlag` lines are applied
after the LAGs exist and before spanning tree.

## What was measured

- **One partner, two switches.** The Nexus bundled its link to the SX2 and its
  link to the TX as one LAG. Both links showed the same partner, system
  `02:a8:eb:93:f6:50` and key 263 (0x100 + 7). The SX2 was primary, the TX
  secondary, and both heard each other over the peer-link and the heartbeat.
- **Traffic, every way.** Between the Nexus, both peers and the orphan AS5610,
  in both directions, 50 of 50 pings each time across twelve runs, and no
  duplicates.
- **The device's link to one peer lost.** The SX2's link to the Nexus shut and
  restored under 20-per-second pings. The AS5610, on the SX2 only, kept
  reaching the Nexus across the peer-link and out of the TX's half: 11 of 400
  lost across the failure and the recovery. The TX's own pings lost nothing.
- **The peer-link lost, the peer alive.** The secondary disabled its half, and
  the Nexus dropped that link and carried on over the primary's. 0 of 500
  lost. With the peer-link back, the half came back and the Nexus bundled it
  again.
- **The peer dead.** The TX's datapath killed. The SX2 carried on alone, still
  presenting the same LACP system, so the Nexus did not renegotiate. 0 of 1200
  lost over a minute. When the TX came back and was configured again, the pair
  re-formed and the Nexus bundled both links again.

## How it works

**The pair talks over the peer-link.** Every second each switch sends its
priority, its MAC, the shared LACP system and which of its MLAG ids are up. It
sends at once when one of those changes. The frames go to
`01:80:c2:00:00:0e` with ethertype 0x88b5, a destination the chips' default
L2 cache entries already send to the CPU and never forward.

**One LACP system.** An MLAG interface presents:
- the primary's MAC as its LACP system, kept when the peer is lost so the
  device does not renegotiate;
- key 0x100 + its id;
- port numbers offset by 0x800 on the secondary, so no two links of the pair
  share one.

**No copies.** A frame flooded to the peer across the peer-link, and delivered
by the peer to the device already, must not also leave by this side's half. So
flooding from the peer-link's ports to an MLAG id's members is blocked. It
comes off only when the peer has reported its half down for 2.5 s and this
side's half has been up as long: the single-homed case, which alone needs
it. Known unicast crosses the peer-link either way.

**MAC sync.** MACs learned on an MLAG interface are sent to the peer every
second. The peer installs them on its own half of the same interface, or on
the peer-link if that half is down, so traffic for them is forwarded locally
instead of flooded. When a half goes down, what was learned on it is flushed:
traffic for the device floods across the peer-link and is learned again
there, instead of vanishing into a trunk with no members.

**Split brain.** The peer-link lost but the heartbeat still answering means
both switches are alive and cannot coordinate. The secondary disables its
MLAG members, and the device carries on over the primary's. With the heartbeat
silent too, the peer is gone, and this switch carries on alone.

**Spanning tree** leaves MLAG interfaces and the peer-link alone: they forward.

### Two bugs the hardware found

- ⚠ **`BCM_PORT_FLOOD_BLOCK_ALL` is not "all kinds of flooding".** It is the
  ingress port's egress mask, `ING_EGRMSKBMAP`, and it stops known unicast
  too. The first block used it. A reply the peer had to send across the
  peer-link for this side's half, to the device, was dropped at the wall:
  pings to the Nexus worked, and pings from it failed. The block is now
  broadcast, unknown unicast and multicast only. `BCM_PORT_FLOOD_BLOCK_ALL`
  stays right for [lag.c](lag.md#three-bugs-the-hardware-found)'s blocks
  between members of one LAG, which must never forward to each other at all.
- ⚠ **One flood let through, and the device learns its own MAC.** If a flood
  the peer has delivered also comes back out of this side's half, the device
  receives its own frame on its LAG. It learns its own MAC there, and drops
  every ARP reply to it from then on as going back where it came from. The
  Nexus did this twice:
  - blocking only once this side's half was distributing left a 200 ms window
    behind LACP;
  - blocking unless the peer had reported its half down left a window when
    both halves came up together.

  Hence the 2.5 s settling rule, and an immediate hello on any change.

## Not yet

- **A short window remains** when the second half of an MLAG interface comes
  up: up to one 200 ms tick, until the peer hears of it. It did not bite
  in the tests. A reload delay, holding a new half back until the peer has
  confirmed it, would close it.
- **A shared gateway** is a separate piece: [gateway.md](gateway.md).
- **MAC sync is one-way per MAC.** A synced MAC is static on the receiving
  peer until the next sync that no longer lists it, so a host that moves from
  the dual-homed device to an orphan port on that peer is followed only when
  the other peer's entry for it ages out.
- **Spanning tree does not run on MLAG interfaces or the peer-link.** A loop
  through them is not caught.
- **The heartbeat is IPv4 only.**
