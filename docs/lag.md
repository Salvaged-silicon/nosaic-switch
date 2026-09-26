# Link aggregation

Port-channels: several ports that act as one interface, `po<N>`, static or
negotiated by LACP (IEEE 802.1AX). This is Cisco's `interface Port-channel1`
and Linux's bond. switchapi 1.3. The chip forwards across the members; the
datapath decides which members those are.

Proven on 2026-09-25, between NOSaic switches, with traffic:

| board | chip | datapath | static | LACP | routed | switched (trunk + SVI) | failover |
|---|---|---|---|---|---|---|---|
| [Arista 7050SX2-72Q](../platform/arista-7050sx2-72q/README.md) | Trident2+ | td2p | ✅ | ✅ | ✅ | ✅ | ✅ |
| [Arista 7050TX-64](../platform/arista-7050tx-64/README.md) | Trident2 | td2 | ✅ | ✅ | ✅ | ✅ | ✅ |
| [Edgecore AS5610-52X](../platform/edgecore-as5610-52x/README.md) | Trident+ | tdp | ✅ | ✅ | ✅ | ✅ | ✅ |
| [Cisco Nexus 3172TQ](../platform/cisco-n3172tq/README.md) | Trident2 | td2 | — | ✅ | ✅ | — | ✅ |

The test bed:
- **SX2 and TX:** two 40G links between them, et52/et53 ↔ et49/et50, run as
  one LAG, first static and then LACP.
- **AS5610:** first a single-member LAG, swp51 against the SX2's et54,
  because that port is on the chip's second module id, where the receive path
  reports it as port 19. Then, once swp1 and swp2 were cabled to the SX2's et3
  and et4 on 2026-09-26, a two-member LAG, po5, in every mode.
- **Nexus 3172TQ:** tested on 2026-09-26 with an LACP LAG of its two 10G
  copper links to the TX, eth1_31/eth1_32 ↔ et31/et32. That is the case the
  40G tests could not cover: a LAG over the TX's external-PHY copper ports.

A dash means "not tested on that board", not "does not work".

The virtual board implements the contract with Linux bonds, and `make
dataplane-test` negotiates LACP with a far-end bond on every build.

## Commands

The same on the Go CLI and the C CLI:

    nosaic lag po1 lacp et52,et53 [rate fast|slow] [mode active|passive] [port-priority <n>]
    nosaic lag po1 static et52,et53
    nosaic lag po1 none
    nosaic lacp system-priority <n>

    nosaic show lags
    nosaic show caps                 lags: yes, 64, up to 16 members each, lacp

`lag` **states the LAG's whole configuration**, the way `switchport` does:
- members the line does not name leave;
- changing `static` to `lacp` keeps the members;
- running the line twice is the same as running it once;
- `none` removes the LAG and frees its members;
- an option the line leaves out goes back to its default.

LACP's settings:

| setting | values | default | what it does |
|---|---|---|---|
| `rate` | `fast`, `slow` | `fast` | how often the partner is asked to send: every second and given up after 3 s, or every 30 s and 90 s |
| `mode` | `active`, `passive` | `active` | a passive end answers an active partner but never starts LACP |
| `port-priority` | 1–65535 | 32768 | which links a partner prefers when it cannot use them all |
| `lacp system-priority` | 1–65535 | 32768 | the switch's, for every LAG |

In network.conf, `lacp system-priority <n>` is applied before any LAG, and
`lag` lines take the same options.

Measured between the 7050SX2 and the AS5610 over po5:
- **Active against passive:** a passive, slow end formed a LAG with an active
  one. The active end saw partner state `0x3c`: the active and short-timeout
  bits clear.
- **Passive against passive, fresh:** two new LAGs never sent a single LACPDU.
- **A running LAG turned passive at both ends:** exactly one LACPDU went out,
  to tell the partner, and both ends timed out.

`show lags`:

    LAG  MODE  ACTIVE     INACTIVE
    po1  lacp  et52,et53  -

A member is **active** while it is in the chip's trunk and carrying traffic.
- On a static LAG, that is while it has link.
- On an LACP LAG, it is also while the partner agrees: it is in sync,
  collecting and distributing.

A LAG's name goes wherever a port name does:
- `switchport po1 trunk 10,20`;
- `ip addr add ... dev po1`;
- OSPF, which picks it up from its `network` statements like any other
  interface.

A member's own name goes nowhere while it is a member:
- `switchport et52 ...` is refused with "et52 is a member of po1";
- its tap, `et52`, stays but falls silent.

A worked example, a routed LAG running OSPF:

    ip addr del 10.101.101.81/29 dev et52
    nosaic lag po1 lacp et52,et53
    ip addr add 10.101.101.81/29 dev po1
    ip link set po1 mtu 1600

And the same LAG as a trunk:

    nosaic vlan add 300
    nosaic switchport po1 trunk 300
    nosaic svi add 300
    ip addr add 10.99.30.1/24 dev vlan300

## In network.conf

    lag po1 lacp et52,et53
    iface po1 10.101.101.81/29 mtu 1600
    switchport po2 trunk 10,20

- **Order:** `apply-network.sh` applies every `lag` line before any `vlan`,
  `switchport` or address line, so `po1` exists by the time anything names it.
- **Reconcile:** the timer applies the lines again every 30 s, like the VLAN
  lines ([vlan.md](vlan.md#in-networkconf)).
- **Restart:** the same caveat applies. A datapath restart forgets every LAG
  until the next reconcile pass.

## What was measured

All of this was between the 7050SX2 and the 7050TX-64 over their two 40G
links, unless the item says otherwise.

- **LACP negotiates.** Both ends reached actor and partner state `0x3f` (activity, short timeout, aggregation, sync, collecting,
  distributing). Each named the other's system id correctly: the switch's own
  derived MAC.
- **The chip forwards; the CPU does not.**
  - 600 pings from the AS5610 went through the SX2 into po1 with the SX2's po1
    tap counters flat, apart from OSPF hellos.
  - The same held with po1 switched: routed into VLAN 300 on the trunk, and
    back.
  - The same held on the AS5610's own trunk.
- **The hash spreads, once it is RTAG7.** With the legacy hash, 20 flows
  through the Nexus all left by one member (see below). With RTAG7, the flows
  that crossed po3 split five and five: 92 frames out of eth1_31 and 100 out
  of eth1_32. The earlier SX2 split, 300 frames on et52 and 506 on et53, was
  the legacy hash splitting on the parity of the source addresses.
- **Failover under traffic.** Six flows at 50 ms intervals, with one member
  taken down and brought back:
  - routed LAG: 1 to 6 of 400 pings lost per flow, an outage of at most about
    300 ms;
  - switched LAG: 11 to 13 of 300 lost, about 0.6 s, the extra being the
    MACs relearned;
  - both ends stopped distributing on the member and took it back when its
    link returned.
- **The Nexus** negotiated LACP with the TX, carried OSPF over po3, and
  forwarded transit traffic into it in the chip. Shutting eth1_32 under four
  flows cost the two hashed to it 20 of 300 pings, about 1 s, and the other two
  nothing.
- **The AS5610** negotiated LACP on swp51, carried OSPF over po2, routed
  transit traffic into its trunk in the chip, and dropped and recovered the
  member when the far end's port went down and up. On po5 (swp1, swp2):
  - LACP, and static;
  - transit from the TX routed into the trunk, spread over both members;
  - a member shut under four flows: 4 of 300 pings lost on the two hashed to
    it, about 200 ms, and it came back when the link did;
  - po5 as a VLAN trunk with SVIs, fresh ARP every time, and transit routed
    into the VLAN.
- **`lag ... none`** gave every member back as a routed port. The lab's
  original per-port adjacencies all came back.

## How it works in the chip

**A LAG is an interface, routed until it joins a VLAN**, exactly like a port
([vlan.md](vlan.md#the-model-every-port-is-routed-until-it-is-switched)).

| | routed LAG | switched LAG |
|---|---|---|
| members are in | the LAG's own service VLAN, 4000 + N, untagged | the LAG's user VLANs, as a set |
| what they receive reaches | the tap `po<N>` | the VLAN's SVI |
| next hops leave by | the trunk: an egress object with `BCM_L3_TGID` | the trunk, found in the L2 table (learned on the TGID) |
| member learning | off | on |

**The trunk table holds exactly the distributing members.** lag.c restates it
with `bcm_trunk_set` every time that set changes:
- a static member distributes while it has link;
- an LACP member distributes once selected, with the partner in sync and
  collecting.

It does not leave this to the chip:
- ⚠ On the Tridents, hardware linkscan takes a member whose link drops out
  of the trunk, but **never puts it back**.
- The AS5610 scans links in software and removes nothing.

So lag.c keeps two watches: its own 200 ms tick, and a linkscan callback that
restates the table after a flap too short for the tick to see.

⚠ **On the 7050TX-64's copper ports, a dead far end can still read as link
up.** When the Nexus shut eth1_32, the TX never logged et32 going down: its
external PHY kept reporting link. LACP pulled the member anyway, once the
partner's LACPDUs stopped arriving (three seconds). A **static** LAG on those
ports has no such backstop and keeps hashing traffic into the dead link. Use
LACP on copper.

**The trunk hash is RTAG7:** `BCM_TRUNK_PSC_PORTFLOW`, a CRC32 over the MAC
addresses, the IP addresses, the protocol and the L4 ports, set up the same
way in both of the chip's hash banks.
- ⚠ The legacy `BCM_TRUNK_PSC_SRCDSTIP` XORs the two addresses and keeps the
  low bits, so with two members it is the parity of source XOR destination.
- Measured on the Nexus: five sources, all odd, to four destinations, all
  even, and every packet left by the same member.
- Point-to-point /29s and /31s make that the normal case, not the unlucky one.
- A datapath whose chip refuses the RTAG7 controls says so at start-up and
  falls back to the legacy hash.

ECMP is unaffected: it keeps its own inputs, and the enhanced-ECMP bit is not
set.

**LACP runs in the datapath:**
- a receive callback ahead of the tap bridge takes slow-protocol frames
  (ethertype 0x8809);
- a thread sends LACPDUs every second and expires a partner that misses three;
- LACPDUs go to `01:80:c2:00:00:02`, which the SDK's default L2 cache entries
  already send to the CPU.

The receiving port comes from the punt header's (module, port), translated to
a local port. On the AS5610 swp51 arrives as module 1, port 19.

**The CPU's own frames go to one member.** A LAG's tap, or an SVI whose VLAN
contains a LAG, sends each frame to one distributing member, chosen by a hash
of the MAC addresses:
- sending to all of them would hand the partner one copy per link;
- a flood sends one copy per LAG, not one per member.

### Three bugs the hardware found

- ⚠ **The chip floods back into the trunk a frame came in on.** A broadcast
  that arrives on one member is flooded to one member of each trunk in the
  VLAN, including its own. The TX sent the SX2's ARP broadcasts straight back
  to it over the other link. The SX2 then learned its own MAC on the trunk,
  and every unicast frame for it was dropped as going back out of the port it
  arrived on. Pings that had worked stopped, and OSPF sat in ExStart. Every
  pair of members now blocks flooding to each other
  (`bcm_port_flood_block_set`), and a routed LAG's members do not learn.
- ⚠ **The flood mask ate its own pick.** Trimming a flood to one member per
  LAG removed each member and added the chosen one back, in one walk. When the
  walk reached the chosen one afterwards, it took it out again. A broadcast
  whose hash chose the LAG's later port went nowhere, so an SVI's ARP failed
  while its unicast worked. It looked intermittent, because unicast
  re-validation kept some neighbours alive.
- ⚠ **A neighbour that moved stayed where it was.** The chip's host entry for
  a directly attached neighbour is keyed by IP alone. When the SX2's address
  moved from being behind the AS5610's swp1 to behind po5, adding it again
  failed with EXISTS, quietly, on every poll. Pings from the AS5610 itself
  worked, and every routed packet for the SX2 left by swp1, into nothing. Not
  a LAG bug: any address moved between two of a switch's interfaces did it.
  l3sync now replaces the entry and forgets where it used to be.

## Rules the datapath enforces

- **Up to 64 LAGs, `po1` to `po64`, and 16 members each.** Sixteen is the
  Trident+'s trunk limit, and the smaller of the three chips' limits.
- **A member must be a routed port.** A port in a VLAN is refused: take it
  out with `switchport <port> none` first.
- **A port is in one LAG at most.**
- **Service VLANs 4001–4064 are reserved** for routed LAGs, and `vlan add`
  refuses them.

## Not yet

- **MLAG** is a LAG whose members are on two switches: [mlag.md](mlag.md).
- **A member's MTU is not managed.** Set `po<N>`'s MTU to match what the
  neighbour's LAG has. OSPF refuses an adjacency across a mismatch.
- **Marker PDUs are absorbed, not answered.**
- **The helix4 datapath** (AS4610) compiles lag.c but has not been built or
  run with it.
