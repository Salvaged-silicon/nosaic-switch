# VLANs and routed VLAN interfaces

802.1Q VLANs on front-panel ports, access and trunk, and SVIs: a routed
interface for a whole VLAN, `vlan<VID>`, which is what Cisco calls
`interface Vlan10`. switchapi 1.2. The switching and the routing both happen
in the chip.

Proven on 2026-09-24 on all three Broadcom families in the lab, between
NOSaic switches, with traffic:

| board | chip | datapath | access | trunk + native | routed into the VLAN by the chip |
|---|---|---|---|---|---|
| [Arista 7050SX2-72Q](../platform/arista-7050sx2-72q/README.md) | Trident2+ | td2p | ✅ | ✅ | ✅ |
| [Arista 7050TX-64](../platform/arista-7050tx-64/README.md) | Trident2 | td2 | — | ✅ | — |
| [Edgecore AS5610-52X](../platform/edgecore-as5610-52x/README.md) | Trident+ | tdp | — | ✅ | ✅ |
| [Cisco Nexus 3172TQ](../platform/cisco-n3172tq/README.md) | Trident2 | td2 | — | — | — |

A dash is "not tested on that board", not "does not work". The 3172TQ runs
the same td2 datapath as the 7050TX-64 and reports the capability; nothing
has been driven through it yet. The virtual board implements the same
contract with a Linux bridge, and `make dataplane-test` drives it with
traffic on every build.

## The model: every port is routed until it is switched

A front-panel port starts as a **routed** port, as it always has: a tap
(`swp1`, `et49`, `eth1_54`) that takes addresses, runs OSPF and forwards in
the chip through its own router interface.

It becomes a **switched** port the moment it joins a VLAN, and goes back to
routed when it leaves the last one. Nothing else marks the difference: there
is no `no switchport` to type, and no port is half of each.

|  | routed port | switched port |
|---|---|---|
| member of | its own private service VLAN, alone | the user VLANs it was put in |
| untagged frames go into | the service VLAN | its native VLAN, if it has one; otherwise they are dropped |
| its tap (`swp1`) | sends and receives | is silent: an SVI speaks for the VLAN instead |
| addresses go on | the port | `vlan<VID>` |

An **SVI** is `vlan<VID>`, an interface the datapath makes for a VLAN. It
takes addresses like any other interface, OSPF runs over it, and the chip
routes through it: a packet arriving on any port and routed to a neighbour
behind the SVI leaves by whichever member port that neighbour is on.

## Commands

Identical on the Go CLI (the Aristas and the Nexus) and the C CLI (the
AS5610):

    nosaic vlan add <vid>
    nosaic vlan del <vid>

    nosaic switchport <port> access <vid>
    nosaic switchport <port> trunk <vid>[,<vid>...] [native <vid>]
    nosaic switchport <port> none

    nosaic svi add <vid>
    nosaic svi del <vid>

    nosaic show vlans
    nosaic show caps                 vlans / svis, contract 1.3

`switchport` **states the port's whole membership**, not a change. Whatever
the line does not name is removed, so

    nosaic switchport swp49 trunk 10,20 native 1

leaves swp49 in 1 (untagged) and 10 and 20 (tagged) and nothing else, however
it was before. Running it a second time does nothing. That is what lets the
same line sit in a configuration file that is applied over and over.

`show vlans`:

    VLAN  SVI      UNTAGGED  TAGGED
    100   vlan100  -         et52
    200   vlan200  et52      -

A worked example, access ports and a routed VLAN:

    nosaic vlan add 10
    nosaic switchport swp1 access 10
    nosaic switchport swp2 access 10
    nosaic svi add 10
    ip addr add 10.0.10.1/24 dev vlan10

swp1 and swp2 now switch between each other in the chip, and anything routed
to 10.0.10.0/24 leaves by whichever of them the destination is behind.

### Rules the datapath enforces

- **One native VLAN per port.** An untagged membership replaces the port's
  previous one: an untagged frame can only go into one VLAN.
- **A VLAN with an SVI cannot be deleted.** `svi del` first: an interface
  with addresses and nothing to route for is not a state worth allowing.
- **An SVI needs its VLAN.** `svi add` on a VLAN that does not exist is
  refused.
- **The datapath's own VLANs are reserved.** Every routed port has a private
  service VLAN, and `vlan add` refuses those VIDs with a reason:

  | board | reserved |
  |---|---|
  | 7050SX2-72Q, Nexus 3172TQ | 1001–1054, one per declared port (`tap_<port>=` in `asic.conf`) |
  | 7050TX-64 | 1001–1052, likewise |
  | AS5610-52X | 3300 + port, for **every** port on the chip, tapped or not |
  | every board | 4001–4064, one per LAG ([lag.md](lag.md)) |

  The per-port VLANs are what the `tap_` lines in each board's `asic.conf`
  (`taps.conf` on the AS5610) declare. VLAN 1 is allowed.

A LAG, `po<N>`, is an interface like a port: `switchport po1 trunk 10,20`
puts all its members in those VLANs as one, and `show vlans` lists it by its
own name. [lag.md](lag.md).

## In network.conf

The same statements, one per line:

    vlan 10
    vlan 20
    switchport swp1 access 10
    switchport swp49 trunk 10,20 native 1
    iface vlan10 10.0.10.1/24 mtu 1600

`apply-network.sh` hands each line to the CLI before it configures any
address. An `iface vlan<N>` line makes SVI N first. `vlan<N>` does not exist
until the datapath makes it, and the address loop would otherwise wait out
its whole deadline for it. The reconcile timer applies them again every 30 s.

⚠ **A datapath restart forgets every VLAN.** nosd keeps its VLANs in the chip
and in memory, the same as the taps and the addresses on them, and the next
reconcile pass puts them back. So for up to 30 s after nosd restarts, switched
ports are routed ports with no addresses and SVIs do not exist. That is the
same window routed ports already have for their addresses.

⚠ **Removing a line does not undo it.** Like `iface` and `route`, the file
states what should exist and never deletes. `nosaic vlan del`,
`switchport <port> none` or `svi del` takes it away on the running switch.

⚠ **Not settings.** Access lists persist as `acl_<seq>` settings through
`config set` (see [acl.md](acl.md)). VLANs persist through network.conf, next
to the addresses and routes they belong with. The two are different on
purpose for now, and one of them should probably move.

## The virtual board

`internal/nosd/virt` implements the same contract with a Linux bridge that has
VLAN filtering on (`br0`, `vlan_default_pvid 0`):

- a VLAN exists when the bridge itself is a member of it;
- a switched port is enslaved to the bridge;
- an SVI is a vlan device on the bridge.

All of that state is read back from the kernel, so a restarted nosd finds what
it had. It needs the `bridge` tool from iproute2. Without it, VLANs are
refused as unsupported rather than faked.

`make dataplane-test` checks, on every build:
- the contract suite passes;
- an access host reaches a tagged trunk host;
- a host in another VLAN on the same subnet is refused;
- an SVI answers both.

## How it works in the chip

`datapath/common/vlan.c`, shared by every Broadcom datapath.

**Joining a VLAN** takes the port out of its service VLAN, turns on ingress
filtering and learning, and sets FORWARD in the VLAN's own spanning-tree
group. Ingress filtering is what stops untagged frames on a trunk that has no
native VLAN from falling into the old PVID and reaching the routed tap. The
spanning-tree group has to be the VLAN's own because the port's default group
is not the one that decides. **Leaving the last one** reverses all of it and
flushes the port's learned MACs.

**The chip switches.** Learning, flooding and forwarding between members
happen in silicon; the CPU sees none of it. On a 7050SX2, 20 pings switched
between two access ports left the CPU's counters exactly where they were.

**An SVI is a tap and a router interface.** The tap `vlan<VID>` is kept apart
from the port taps, so nothing that means "front-panel port" ever sees it. Its
MAC comes from the switch's own base ([switch-derived MACs](#addresses)). The
CPU joins a VLAN only while the VLAN has an SVI, so a VLAN nobody routes for
punts nothing.

**Receive is by VLAN.** A punted frame goes to the tap its VLAN tag names: a
routed port's service VLAN, or an SVI. The source port is only a fallback,
which also sidesteps the 40G ports' source-port numbering.

**An SVI's next hop has no fixed port.** l3sync looks the neighbour's MAC up
in the chip's L2 table to find the port, and rewrites the egress object in
place when the neighbour moves. Routes and host entries follow without being
touched. The SVI's own transmit does the same lookup, and floods to the
members when the MAC is unknown.

⚠ **An L2 entry's port is (module, port), not a logical port.** The Trident+
has more ports than one module ID covers. On the AS5610, a neighbour behind
swp51 (logical 51) is reported as port 19 on the second module, and an egress
object built for "19" sends routed traffic out of a dark 10G port.
`nosaic_l2_port()` translates. The same split is why the 7050SX2's 40G
source-port numbers read 17, 19 and 20 for 49, 51 and 52.

**Every address of an interface is sent to the CPU.** Each IPv4 address on an
interface, not only the first, gets a to-CPU host entry, and addresses that go
away lose theirs. Before 2026-09-24 only the primary did, and only once: a
secondary answered ARP and dropped everything else sent to it, and an SVI given
back to a VLAN with a new address never received anything.

## Addresses

Every tap and SVI MAC is `02:<low four bytes of the switch's base MAC>:<index>`:
- locally administered;
- distinct per port within a switch;
- distinct between switches.

At boot, `/etc/nosaic/switch-mac.sh` works out the base, before the datapath
starts. It takes the first of:
- the board's identity (`nosaic platform mac`, the ID PROM on the 3172TQ);
- the `mac` on eth0's line in this switch's `network.conf`;
- eth0's own address, if it is a real one: not multicast, not locally
  administered, and not a vendor OUI with the all-zero suffix an unprogrammed
  NIC reports.

It writes the result to `/run/nosaic/base_mac`. An explicit `tap_mac_base`
setting still wins. With none of these, the datapath says so once and falls
back to `02:00:00:00:00:xx`, which is the same on every NOSaic switch.

## Contract

switchapi 1.2 (`internal/switchapi`). Every datapath (the in-memory reference,
virt, and the C daemons through `/run/nosd.sock`) implements:

| op | call | |
|---|---|---|
| `vlan.add` / `vlan.del` | `AddVLAN` / `DelVLAN` | DelVLAN refused while an SVI exists |
| `vlan.port` | `SetPortVLAN(port, vid, tagged)` | untagged = native, one per port |
| `vlan.port.del` | `DelPortVLAN(port, vid)` | the last one makes the port routed |
| `vlans` | `VLANs()` | VID, members (tagged or not), SVI |
| `svi.add` / `svi.del` | `AddSVI` / `DelSVI` | interface `vlan<vid>`, addresses via AddAddress |
| `capabilities` | `Capabilities()` | `VLANs`, `MaxVLANs`, `SVIs` |

`nosaic verify contract` runs the conformance suite against the live datapath
over its socket. It checks that memberships read back, that the native-VLAN
rule holds, that tagged beside native is a trunk, and that an SVI takes an
address and blocks DelVLAN. It creates and removes VLANs 100 and 200 and an
address on the first port, so it is for a switch being brought up. On the
hardware datapaths it currently reports three failures that are not about
VLANs: `port.admin` and `l3.addr.*` are not served by the C daemons, although
they claim L3.

## Not done

- **Only access, trunk and native.** No private VLANs, no QinQ, no voice
  VLAN, and no per-VLAN MTU. An SVI's MTU is whatever `iface ... mtu` or
  `ip link` sets.
- **No spanning tree.** Every member forwards. Two switched ports looped
  together storm, the same as they would on any switch with STP off.
- **No MLAG or port channels.** A neighbour behind a trunk group in the L2
  table is treated as unresolved.
- **No `l2.fdb`.** The chip learns, but the MAC table is not served over the
  contract, so `L2Learning` is false.
