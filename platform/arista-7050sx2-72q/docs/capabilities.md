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

## Features the silicon has and NOSaic does not use

Ordered by how much they matter to a switch being a switch.

| feature | hardware | NOSaic | note |
|---|---|---|---|
| **ECMP** | up to **128-way** on this model | 1 next hop per prefix | `l3sync` takes a single next hop; the datasheet calls out 128-way for the 7050SX2-72Q specifically |
| **VRF** | yes | none | wanted immediately: the management pin exists because in-band and out-of-band share one table |
| **Cut-through** | 550 ns | store-and-forward | EOS sets `cut_through=1`, which is an Arista property absent from OpenBCM 6.5.24 — so this needs finding, not copying |
| **CoS queues** | 8 per port | SDK default | `bcm_num_cos=8` in EOS |
| **ACLs** | ingress + egress, L2/L3/L4, with counters | field processor used only to punt | the FP groups exist; nothing exposes them |
| **Mirroring** | 4 active sessions, filtered | none | |
| **PFC / ETS** | yes | none | |
| **sFlow** | yes | none | |
| **LANZ** | microburst detection | none | Arista software, not a chip feature |
| **VLANs** | 4096 | one per port | the tap bridge's design, and correct for a router |

## What this list is not

It is not a roadmap. Most of these are features a switch grows once it has
users, and NOSaic has none. Two are different:

**ECMP** and **VRF** are both load-bearing for what this board already does.
ECMP because the box has two uplinks of equal cost and takes one; VRF because
management traffic currently follows a learned route into the CPU path unless
pinned by hand.

The table sizes are worth doing because they are nearly free — two properties,
already known-good on this silicon because the vendor ships them — and because
16K host routes is a number somebody will hit without understanding why.
