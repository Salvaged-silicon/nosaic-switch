# Virtual gateway

A shared gateway address on an SVI. Both switches of an [MLAG](mlag.md) pair
answer for it with the same MAC, and both route for it, so a host's default
gateway outlives either switch. Whichever switch a host's traffic reaches
routes it in the chip. This is what Arista calls VARP and Cumulus calls VRR:
active-active, with no election and no failover to wait for. switchapi 1.6.

Proven on 2026-09-26 on the MLAG test bed:
- **Peers:** a 7050SX2-72Q and a 7050TX-64.
- **Dual-homed:** a Nexus 3172TQ.
- **Gateway:** `10.99.40.254` on vlan400 of both peers.

The AS5610's datapath (tdp) implements it too, untested. The virtual board
refuses it.

## Commands

    nosaic gateway add <svi> <address/len>
    nosaic gateway del <svi> <address/len>
    nosaic gateway mac <mac>
    nosaic show gateways

On both switches of the pair, identically:

    nosaic gateway add vlan400 10.99.40.254/24

- **Each SVI keeps its own address.** Here the SX2 has `10.99.40.1` and the TX
  `10.99.40.2`, and those are what OSPF and the switches' own traffic use.
  The gateway is an extra address.
- **The virtual MAC** is `00:00:5e:00:01:01` unless set: the IANA VRRP MAC for
  router 1. It must be the same on both switches of a pair. A second pair in
  the same VLANs needs a different one.
- **IPv4 and IPv6** (switchapi 1.11, `Capabilities.VirtualGateway6`). An
  IPv6 gateway is a global or unique-local address, never link-local:
  `nosaic gateway add vlan400 2001:db8:40::fe/64`. One virtual MAC serves
  both.

`show gateways`:

    SVI      ADDRESS          MAC
    vlan400  10.99.40.254/24  00:00:5e:00:01:01

In network.conf:

    gateway mac 00:00:5e:00:01:01
    gateway vlan400 10.99.40.254/24
    gateway vlan400 fd00:40::fe/64

These lines are applied after the SVIs exist, the MAC before the addresses.

## What was measured

- **Resolved to the virtual MAC.** The Nexus ARPed for `10.99.40.254` and got
  `00:00:5e:00:01:01`, and kept it.
- **Routed through it.** 20 of 20 pings to each peer's loopback and to the
  AS5610 behind them, with the Nexus's routes pointing at the gateway.
- **A link lost.** With either peer's link to the Nexus shut and restored
  under traffic, the other peer routed for the gateway.
- **A peer lost.** The TX's datapath killed under a 20-per-second ping from the
  Nexus, through the gateway, to the SX2's loopback: 1200 of 1200, no
  duplicates. The SX2 routed for the gateway alone.

⚠ **The gateway is only half the path.** The same test to the AS5610 lost
three quarters of a minute's pings with the SX2 killed. The requests were
routed through the TX without trouble, but the AS5610 sent its replies
towards the SX2 until its OSPF dead timer, 40 s, gave up on it. A gateway
keeps the first hop alive. How quickly the rest of the network stops using a
dead switch is up to its routing protocol, and faster hellos or BFD are what
shorten it.

## How it works

- **The chip.** A MY_STATION entry for the virtual MAC, so a frame sent to it is
  routed in the chip, exactly as one sent to the SVI's own MAC is. Station
  entries are not per VLAN, so one entry serves every gateway.
- **ARP.** The datapath answers an ARP request for a gateway address itself,
  from the virtual MAC, and does not pass it to the kernel. The kernel would
  answer with the SVI's own MAC, and a host would keep whichever answer came
  last. Both switches answer, so the host hears the same MAC twice. A
  gratuitous ARP goes out when a gateway is added.
- **The kernel.** The address goes on the SVI as a /32, so the kernel answers
  pings to it and forwards anything the chip punts. A /32 never becomes the
  interface's primary address, so the kernel's own traffic still comes from
  the SVI's own address. A frame to the virtual MAC that the chip punts is
  handed to the tap with its destination rewritten to the tap's MAC, or the
  kernel would drop it as someone else's.
- **No macvlan.** None of the switch kernels has one, and the kernel is
  outside the A/B slot.

### A bug the hardware found

- ⚠ **The kernel ARPs from the gateway address.** Answering a ping to
  `.254`, the kernel sources the reply from `.254`. If it has to ARP for the
  host first, its request says "`.254` is at *the SVI's own MAC*", and the
  host believes it over the virtual MAC. The Nexus resolved the gateway to
  the 7050SX2's SVI MAC this way. `arp_announce` is set to 2 on every SVI with
  a gateway, so the kernel's requests carry the SVI's own address instead.

## IPv6

The same three pieces, with neighbour discovery for ARP:

- **Neighbour discovery.** tapbridge answers a solicitation for a gateway
  address itself, with an advertisement from the virtual MAC flagged router
  and override, and passes nothing to the kernel. A solicitation from `::`
  (a host's own duplicate address detection) is answered to all nodes. When
  a gateway is added, an unsolicited advertisement goes to all nodes, as the
  gratuitous ARP does for IPv4.
- **The kernel.** The address goes on the SVI as a /128, with duplicate
  address detection off, because the other switch of the pair answers for
  it, and the kernel would call that a duplicate and refuse the address. It
  is also **deprecated** (preferred lifetime 0), so the kernel never picks it
  as the source of its own traffic.
- ⚠ **Deprecated is not enough.** Answering a ping to the gateway address,
  the kernel replies from it. If it has to find the host first, Linux takes
  the solicitation's source from the packet that triggered it, deprecated or
  not, and the solicitation carries the SVI's own MAC. The Nexus resolved the
  gateway to the SX2's SVI MAC this way. IPv4's `arp_announce` has no IPv6
  twin, so tapbridge rewrites a solicitation or advertisement the kernel
  sends from a gateway address to leave from the SVI's own link-local
  address, with its checksum made good.

Measured on 2026-09-27 on the same pair, with `fd00:40::fe/64` on vlan400:
- The Nexus resolved the gateway to `00:00:5e:00:01:01` and kept it.
- 20 of 20 pings through the gateway to each peer, and 20 of 20 in transit
  through the chip to the AS5610's IPv6 loopback.
- With the TX's datapath killed under 20 pings a second through the gateway
  to that loopback: 1200 of 1200, no duplicates.

## Not yet

- **One virtual MAC per switch**, not one per gateway.
- **A peer that has just rebooted attracts traffic before its routes are
  back.** Its half of the MLAG interface comes up as soon as LACP agrees,
  and whatever the device hashes to it is routed with whatever it has
  learned so far. Set MLAG's `reload-delay` ([mlag.md](mlag.md)) on a pair
  that routes: it keeps the half out of service until the routing has had
  time to converge. When only a link comes back, the loss is now about 0.1 s;
  it was about a second.
