# What this switch can do, and what NOSaic does with it

The hardware figures are Arista's, from the 7050X architecture whitepaper and
the 7050SX datasheet. The delivered figures are read off the running switch.
The gap is not a complaint about either — it is the work list, in the order the
silicon suggests rather than the order things happened to break.

## Forwarding tables

The 7050X carries a **Unified Forwarding Table**: 256K entries in four banks,
each bank allocable to MAC addresses, host routes or LPM prefixes. Each
forwarding element also has a dedicated table the UFT augments. So these
maxima are alternatives, not a total:

| | hardware maximum | NOSaic today | measured how |
|---|---|---|---|
| MAC address table | 288K | SDK default | not read back |
| IPv4 host routes | 208K | **16,384** | `host 22/16384` from the chip |
| IPv4 LPM routes | 144K | **8,192** | `CHIP route 43/8192` |
| IPv4 multicast | 104K | unused | |
| IPv6 host routes | 104K | shares the v4 table | |
| IPv6 LPM routes | 77K | shares the v4 table | |

Two of those are one property each. EOS sets `l3_mem_entries=147456` and
`l2_mem_entries=163840`, which is how it carves the UFT; NOSaic sets neither
and gets the SDK's defaults. **A ninefold difference in host route capacity
between us and the vendor, from a line of configuration.**

The LPM figure is different in kind. Reaching 144K needs UFT banks given to
LPM *and* `l3_alpm_enable`, the algorithmic LPM that defaults to 0
(`src/soc/esw/trident2.c:5556`). EOS does not set it either, so 8,192 prefixes
may well be what this box runs in production — but that is an inference from a
config dump, not a measurement, and it should be checked before anybody
concludes 8K is enough.

## Features the silicon has, and how much of each NOSaic uses

Ordered by how much they matter to a switch being a switch.

| feature | hardware | NOSaic | note |
|---|---|---|---|
| **ECMP** | up to **128-way** on this model | **yes, in silicon** | `l3sync` builds `bcm_l3_egress_ecmp` groups from netlink multipath routes; proven here 2026-09-11, 75 transit packets split 40/35 across et52/et53. The datasheet calls out 128-way for the 7050SX2-72Q specifically |
| **VRF** | yes | management VRF only, in the kernel | `vrf mgmt table N` in network.conf puts eth0 in its own table (proven on this board 2026-09-24). Front-panel VRFs in the chip are not started |
| **Cut-through** | 550 ns | store-and-forward | EOS sets `cut_through=1`, which is an Arista property absent from OpenBCM 6.5.24 — so this needs finding, not copying |
| **CoS queues** | 8 per port | SDK default | `bcm_num_cos=8` in EOS |
| **VXLAN** | routing, bridging and gateway at wire speed | nothing | see below — the SDK driver is already compiled in |
| **ACLs** | ingress + egress, L2/L3/L4, with counters | **ingress IPv4 in silicon, with counters** | `datapath/common/acl.c`; proven here 2026-09-17 (below) and on the AS5610. IPv6 group present (`show caps`: 4096 rules) but not traffic-tested here. See [docs/acl.md](../../../docs/acl.md) |
| **Mirroring** | 4 active sessions, filtered | none | |
| **PFC / ETS** | yes | none | |
| **sFlow** | yes | none | |
| **LANZ** | microburst detection | none | Arista software, not a chip feature |
| **VLANs** | 4096 | **access, trunks, SVIs, QinQ** | [docs/vlan.md](../../../docs/vlan.md); a routed port still gets a VLAN of its own. QinQ (802.1ad) as the provider, proven here 2026-09-28 |
| **LAG** | yes | **static and LACP** | [docs/lag.md](../../../docs/lag.md), proven here 2026-09-25 |
| **Spanning tree** | yes | **RSTP, BPDU and root guard** | [docs/stp.md](../../../docs/stp.md); root guard proven on et3 2026-09-27 |
| **MLAG** | Arista software, not a chip feature | **yes, with a virtual gateway (IPv4 and IPv6)** | [docs/mlag.md](../../../docs/mlag.md), [docs/gateway.md](../../../docs/gateway.md); peered with the 7050TX-64 and with the AS5610 |
| **Port LEDs** | green, amber, blink | **green / amber** | on the SCD; blink deliberately unused — it would mean traffic, and traffic means a per-port counter sweep every interval |
| **Chassis lamps** | status, PSU1, PSU2, fan, blue beacon | **yes** | on the StandbyCpld behind the SCD's SMBus; driven from the thermal loop's own measurements |

## What this list is not

It is not a roadmap. Most of these are features a switch grows once it has
users, and NOSaic has none. Two are different:

**ECMP** and **VRF** were both load-bearing for what this board already does,
and both are now in: ECMP because the box has two uplinks of equal cost and
took one, VRF because management traffic followed a learned route into the CPU
path unless pinned by hand. What is left of VRF is front-panel VRFs in the chip.

The table sizes are worth doing because they are nearly free — two properties,
already known-good on this silicon because the vendor ships them — and because
16K host routes is a number somebody will hit without understanding why.

## VXLAN, in more detail

Worth its own section because the usual reason not to have a feature — the
silicon cannot do it, or the SDK does not expose it — does not apply here.

The datasheet lists *"VXLAN routing, bridging and gateway"* and a *"wire-speed
gateway between VXLAN and traditional L2/3 environments"*. That is confirmed
below the marketing: `EGR_VXLAN_CONTROL` and the VXLAN header registers are in
the SDK's own BCM56860 chip database (`src/soc/mcm/bcm56860_a0.c`), and
`src/bcm/esw/trident2/vxlan.c` is 16,566 lines with 45 entry points, already
compiled into the OpenBCM build NOSaic links against.

NOSaic has none of it. No VXLAN, no tunnel, no VTEP anywhere in the tree, and
`switch-api` has no tunnel concept at all. At contract 1.13 it covers ports,
VLANs and QinQ, FDB and MAC aging, LAG, spanning tree, MLAG and the virtual
gateway, routes, next hops, ACLs, counters and SFPs. QinQ's outer tag is not
a tunnel in this sense: nothing is encapsulated in IP.

**One caveat before anybody plans around it.** The 7050X architecture
whitepaper says L3 VXLAN routing requires *recirculation* — the packet takes
multiple passes through the forwarding pipeline. So VXLAN bridging is free in
the way line-rate features usually are and VXLAN **routing** is not; it spends
pipeline bandwidth. That should be measured on this board rather than assumed
from the whitepaper.

**Why it is the interesting one to add.** Not because anybody needs it here —
this switch has no users — but because it is the first feature that would
genuinely test whether `switch-api` holds. The design says a real feature
becomes a versioned addition to the contract with the virtual implementation
updated in the same commit, and that if adding one forces the contract to bend
toward Broadcom then the abstraction has already failed. VXLAN is a good test
precisely because it is a multi-vendor feature rather than a Trident2 quirk:
tunnel endpoints, VNI-to-VLAN mapping and overlay next hops are things any
switching ASIC expresses somehow, and `nosd-virt` can answer with Linux VXLAN
interfaces over its veth dataplane so CI still exercises it.

It came behind ECMP and the management VRF on the list, deliberately: both of
those were load-bearing for what this board does, and both are now done.
