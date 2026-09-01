# What this switch can do, and what NOSaic does with it

The hardware figures are Edgecore's, from the AS5610-52X ONIE datasheet. The
NOSaic column is short: nothing runs on this board yet, so this page is a
statement of what is on the table rather than a gap analysis.

It is worth reading beside the 7050SX2's
[capabilities](../../arista-7050sx2-72q/docs/capabilities.md), because the
differences between Trident+ and Trident2+ are the reason the two boards are
not interchangeable.

## The box

| | |
|---|---|
| Switch silicon | Broadcom BCM56846 Trident+, **640 Gbps** |
| Forwarding rate | 960 Mpps |
| CPU | Freescale P2020, dual core **1.2 GHz**, e500v2 |
| Memory | 2 GB DDR3 ECC |
| Storage | 8 MB NOR (firmware) + ~3.8 GB NAND as `/dev/sda` |
| Front panel | 48 × SFP+ 10G, 4 × QSFP+ 40G |
| Latency | 860 ns – 1.2 µs, cut-through, at line rate |
| Typical power | 170 W at line rate |

## Forwarding tables

| | AS5610-52X (Trident+) | 7050SX2-72Q (Trident2+) |
|---|---|---|
| MAC addresses | **128K** | 288K |
| IPv4 routes | **16K** | 208K host / 144K LPM |
| IPv6 routes | **8K** | 104K host / 77K LPM |
| VLANs | 4K | 4K |
| Packet buffer | 9 MB shared | 12 MB |
| Jumbo frames | 9216 bytes | 9236 |

The route figures are the ones to notice. This board's 16K IPv4 routes is not
far off what the 7050SX2 *currently delivers* — its LPM table runs at 8,192
entries and its host table at 16,384, both SDK defaults nobody has tuned. So on
the numbers NOSaic actually achieves today, these two boards are closer than
their datasheets suggest, and the difference is configuration rather than
silicon.

There is no Unified Forwarding Table here. Trident2 introduced the four
allocable banks that let the 7050SX2 trade MAC capacity against host routes;
on Trident+ the tables are what they are.

## Features

Nothing in this column is implemented, because there is no datapath.

| feature | hardware | note |
|---|---|---|
| L2/L3 wire speed | yes | the baseline this port is aiming at |
| Cut-through | 860 ns | |
| Jumbo frames | 9216 | |
| ECMP | yes | Trident+ supports it; the width is not stated in the datasheet |
| ACLs / field processor | yes | **see the blocker below** |
| Per-port LEDs | link, speed, activity | driven by **microcontroller firmware**, not board registers |
| System LEDs | PSU1, PSU2, diagnostic, fans, locator | |
| VXLAN | **no** | Trident+ predates it; the 7050SX2 has it and this board does not |

That last row is the clearest statement of what a generation buys. VXLAN
routing and bridging are on the Trident2+ and absent here, which is why the
`switch-api` capability model has to be real rather than decorative: the same
CLI has to run on both boards and refuse what this one cannot do, rather than
silently doing less.

## The blocker that is not ours

EdgeNOS got an ACL installed in this chip's TCAM, reading back correctly, that
**never matched a packet** — 2000 injected packets flooded through with the
field-processor statistic at zero. It ruled out the bypass enable, the slice
map, the port field select, entry validity and the arming registers, and did
not find the cause.

It is recorded on the [todo](todo.md) because the 7050SX2's punt path is built
on field-processor rules. If the FP cannot be armed on this silicon, the
control plane needs a different mechanism here — and that is worth establishing
early rather than discovering once a datapath otherwise works.
