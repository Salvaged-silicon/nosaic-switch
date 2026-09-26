# Spanning tree

Rapid spanning tree, IEEE 802.1D-2004 clause 17 (what 802.1w became): one
instance for every VLAN, over the switched ports and LAGs. It is what stops a
second cable between two switches, in the same VLAN, from becoming a
broadcast storm, and what lets that second cable take over in a fraction of
a second when the first one fails. switchapi 1.4.

**Spanning tree is off until it is turned on.** A switch upgraded to 1.4
behaves exactly as before until `stp on` is configured.

Proven on 2026-09-26, between NOSaic switches, with traffic:

| board | chip | datapath | loop broken | failover | root moved | LAG as a tree port | transit in the chip |
|---|---|---|---|---|---|---|---|
| [Arista 7050SX2-72Q](../platform/arista-7050sx2-72q/README.md) | Trident2+ | td2p | ✅ | ✅ | ✅ | ✅ | ✅ |
| [Edgecore AS5610-52X](../platform/edgecore-as5610-52x/README.md) | Trident+ | tdp | ✅ | ✅ | ✅ | ✅ | ✅ |
| [Arista 7050TX-64](../platform/arista-7050tx-64/README.md) | Trident2 | td2 | ✅ | ✅ | — | — | — |
| [Cisco Nexus 3172TQ](../platform/cisco-n3172tq/README.md) | Trident2 | td2 | ✅ | ✅ | — | — | — |

A dash is "not tested on that board", not "does not work". The virtual board
runs the Linux bridge's own spanning tree, which is 802.1D rather than
RSTP, and `make dataplane-test` builds a real loop on every build and checks
that it is broken and fails over.

## Where it runs, and where it does not

On **switched interfaces**: ports and LAGs that are members of at least one
VLAN. A LAG is one tree port, whatever its members are doing.

**Never on a routed port.** A routed port sits alone in its own service VLAN
and has no L2 neighbour to loop through, so turning spanning tree on cannot
disturb a routed link or the OSPF on it. Routing protocols keep routed loops
in check; spanning tree is for the switched ones.

## Commands

The same on the Go CLI and the C CLI:

    nosaic stp on [priority <n>] [hello <s>] [forward-delay <s>] [max-age <s>]
    nosaic stp off
    nosaic stp port <port> [edge] [cost <n>] [priority <n>]
    nosaic show stp

`stp on` without a priority is the default, 32768. Lower wins the root
election. The priority goes from 0 to 61440 in steps of 4096. Like `switchport`
and `lag`, each line states the end state: `stp port swp1` with nothing
after it puts swp1 back to the defaults.

- The bridge times default to 802.1D's: hello 2 s, forward delay 15 s, max
  age 20 s. They are checked together, as 802.1D-2004 17.14 requires:
  `2 x (forward-delay - 1) >= max-age >= 2 x (hello + 1)`. A switch that is not
  the root uses the root's times, as the standard says.
- A port's `priority`, 16 to 240 in steps of 16 (default 128), breaks a tie
  between two ports to the same bridge.
- `edge` marks a port as having a host on it, not a bridge. It forwards at
  once instead of negotiating, and stops being an edge port the moment a BPDU
  arrives on it.
- `cost` is the path cost. Left out, it comes from the link speed (802.1D-2004
  table 17-3): 2000 for 10G, 500 for 40G. A LAG's is its active members' total
  speed, so a two-member 10G LAG costs 1000.

`show stp`:

    bridge            8000.023581caae50   priority 32768
    root              1000.02a8eb93f650   cost 500 via swp51
    topology changes  6

    PORT    ROLE        STATE       COST      EDGE
    swp51   root        forwarding  500       -
    po5     alternate   discarding  1000      -

A bridge ID is `priority.mac`, in hex. The MAC is the switch's own.

## In network.conf

    stp on priority 4096
    stp port swp1 edge
    stp port po1 cost 100

`apply-network.sh` applies every `stp` line before any `switchport` line,
wherever they are in the file. With spanning tree on, a port that joins a VLAN
starts out discarding, so a loop in the file is never a loop on the wire, not
even for the seconds a pass takes. The reconcile timer applies them again every
30 s, like the VLAN lines.

⚠ **Take a loop apart before turning spanning tree off.** With both ends of a
loop still in the VLAN, `stp off` makes every port forward, and the loop
storms. `switchport <port> none` on the redundant ports first, then `stp off`.

## What was measured

On the AS5610 and the 7050SX2:

- **A loop, broken.** swp1/swp2 against et3/et4, all four in one VLAN. The
  AS5610 made swp1 its root port and swp2 an alternate, discarding. The
  SX2's two ports were both designated and forwarding. With the loop closed
  and an SVI on each end, the SVI's receive counter moved by 9 frames in
  three seconds. A storm would have been thousands.
- **Root port lost.** et3 shut under a 20-per-second ping: swp2 became the root
  port and forwarded at once, and when et3 came back swp1 took over again.
  Two pings were lost across both changes.
- **Root moved.** The AS5610 set to priority 0 under the same ping: it
  became the root, the SX2 re-elected its own root port and alternate, and one
  ping was lost.
- **A LAG as a tree port.** po5 (swp1+swp2) and swp51 (40G), both in one VLAN
  to the SX2. swp51 won on cost (500 against 1000), and po5 was the
  alternate. With et54 shut, po5 became the root port and forwarded on both
  members, and swp51 took over again when et54 came back. Five of 400 pings
  were lost across both changes.
- **Transit in the chip.** 200 pings routed from the TX through the AS5610
  into the tree's VLAN, with the AS5610's SVI tap counters unchanged.

On the Nexus 3172TQ and the 7050TX-64, over their two 10G copper links:

- **A loop, broken.** The TX was root. The Nexus made eth1_31 its root port
  and eth1_32 an alternate. The TX's et32 was forwarding by agreement within
  six seconds of the loop closing.
- **Root port lost.** eth1_31 shut under a 20-per-second ping: eth1_32 took
  over, and eth1_31 took back its role when it came back. 23 of 400 pings were
  lost, about a second. That is the copper link going down, not the tree: the
  LAG tests over the same cables lost the same second
  ([lag.md](lag.md#what-was-measured)).

## How it works in the chip

**Every user VLAN goes into one spanning-tree group** that the datapath creates
at start-up. An interface's state is written into that group for each of its
ports, and all of a LAG's members get the LAG's state. The service VLANs stay
in the default group, forwarding. A VLAN's forwarding state is its group's,
not the port's default one ([tapbridge.c](../datapath/common/tapbridge.c) has
the story of a port that was forwarding in the wrong group).

**The protocol runs in the datapath** (`datapath/common/rstp.c`):
- BPDUs to `01:80:c2:00:00:00` reach the CPU through the SDK's default L2
  cache entries, even on a discarding port.
- A receive callback ahead of the tap bridge takes them.
- A thread runs the tree on every BPDU and every 100 ms.

It implements the parts of clause 17 a switch needs:
- role selection from priority vectors;
- proposal and agreement, so a point-to-point link forwards in one round trip
  instead of two forward delays;
- edge ports, configured or found: a port that hears no BPDU for three
  seconds after it comes up has a host on it;
- topology changes, which flush learned MACs everywhere else and are passed
  on through the TC flag;
- disputes, a designated port hearing an inferior designated BPDU from a
  port that is already learning (the one-way-link case);
- configuration BPDUs and TCNs, for a neighbour that only speaks 802.1D.

**The CPU's own frames respect the tree.** An SVI's transmit names its ports
and bypasses the chip's state check, so tapbridge leaves blocked ports out
itself.

### A bug the hardware found

- ⚠ **An alternate port has to answer a proposal.** In the standard an
  alternate port is already synced, so it agrees to a proposal at once
  (17.29.3). The first version left it silent. The designated port at the far
  end heard no BPDU at all, and after the migrate time it decided it had a
  host on it: an edge port, on a link to a bridge. Nothing looped, because the
  alternate end was discarding, but an edge port raises no topology changes
  and forwards at once after any change.

## Not yet

- **One instance for every VLAN.** No MSTP, no per-VLAN trees: every VLAN
  blocks on the same ports.
- **No BPDU guard, root guard or loop guard.**
- **MLAG interfaces and the peer-link are left out of the tree** and forward
  ([mlag.md](mlag.md)). A pair that ran the tree as one bridge would catch a
  loop through them.
