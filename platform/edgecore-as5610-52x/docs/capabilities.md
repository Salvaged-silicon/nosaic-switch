# What this switch can do, and what NOSaic does with it

The hardware figures are Edgecore's, from the AS5610-52X ONIE datasheet. NOSaic
runs on this board and forwards through it — installed on its own disk, 10 ports
up, OSPFv2 and OSPFv3 adjacencies with two vendors' routers — so the NOSaic
column below is a gap analysis rather than a wish list: what the silicon offers,
against what is actually driven today.

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

"hardware" is what the silicon can do; "NOSaic" is what is implemented and
observed working on this board. They are different columns on purpose -- the
gap between them is the work, and collapsing it is how a capability model
starts lying.

| feature | hardware | NOSaic | note |
|---|---|---|---|
| L2 forwarding | yes | **yes** | measured: flooding between front-panel ports |
| L3 forwarding | yes | **yes** | routes in DEFIP, transit forwarded in hardware |
| Cut-through | 860 ns | — | not measured here |
| Jumbo frames | 9216 | **no** | taps come up at 1500; nothing plumbs an MTU |
| ECMP | yes | **yes** | 150 transit packets split 80/70 across a pair |
| ACLs / field processor | yes | **yes** | ingress IPv4 and IPv6: port, protocol, addresses, L4 ports; permit and deny, with counters. 1280 v4 and 768 v6 rules. See [docs/acl.md](../../../docs/acl.md) and [below](#the-blocker-that-was-never-the-silicon) |
| VLANs (user-facing) | 4K | **no** | per-port service VLANs only; no VLAN model |
| Link aggregation | yes | **no** | no LACP, no static bonds |
| Storm control / policers | yes | **no** | nothing rate-limits flooding |
| Per-port LEDs | link, speed, activity | **yes** | passthrough microcode in the chip's LED processors: dark / green / amber, blinking on traffic. Speed is not shown — this board's chain has two bits per port and both are spent on colour |
| System LEDs | PSU1, PSU2, diagnostic, fans, locator | read-only | the two registers are known, their bits are not; `platform ledwalk` exists to map them |
| Fans | 3+1, one PWM, 5-bit | **yes** | tracks the hottest sensor, idles at 10/31 |
| Temperatures | max6697, 7 sensors | **yes** | `nosaic platform status` |
| Power supplies | 2, hot-swap | **read** | presence and power-good, active low |
| VXLAN | **no** | — | Trident+ predates it; the 7050SX2 has it and this board does not |

That last row is the clearest statement of what a generation buys. VXLAN
routing and bridging are on the Trident2+ and absent here, which is why the
`switch-api` capability model has to be real rather than decorative: the same
CLI has to run on both boards and refuse what this one cannot do, rather than
silently doing less.

## The blocker that was never the silicon

EdgeNOS got an ACL installed in this chip's TCAM, reading back correctly, that
**never matched a packet** — 2000 injected packets flooded through with the
field-processor statistic at zero. It ruled out the bypass enable, the slice
map, the port field select, entry validity and the arming registers, and did
not find the cause. Its last recommendation was a register diff against a
Cumulus that drops correctly.

**Resolved 2026-09-16, without the diff.** The field processor evaluates live
traffic here under NOSaic's bring-up: the first check was the control plane's
own punt rule for this box's address, which counted exactly the twenty echo
replies sent through it. What EdgeNOS lacked was not a register but the SDK's
own initialisation -- it programmed the TCAM by hand over a minimal init, and
later ran the SDK with `soc_skip_reset=1` over a chip its own code had already
touched. NOSaic resets the chip and runs `soc_init` and `bcm_init` whole, and
`bcm_field` works on top of that with no patch to the SDK. Access lists are
built on it: [docs/acl.md](../../../docs/acl.md) says what they do, and the
[README](../README.md#acls) has the measurements.

One thing on this board is still wrong under the SDK and is worked around
rather than fixed: the ingress-port bitmap gate reaches only one of the chip's
two pipelines. It is on the [todo](todo.md#the-ingress-port-gate-reaches-one-pipeline).
