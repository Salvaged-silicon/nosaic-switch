# Access lists

What `nosaic config set acl_<seq> "<rule>"` does, on a board whose datapath has
a field processor to put it in. Proven on the
[Edgecore AS5610-52X](../platform/edgecore-as5610-52x/README.md#acls) on
2026-09-16; built into the 7050SX2's datapath and not yet tested there.

## A rule is a setting

    nosaic config set acl_10 "deny in swp6 proto icmp src 10.101.101.26/32"
    nosaic config set acl_20 "permit in swp6 proto tcp dst 10.101.101.25/32 dport 22"
    nosaic config unset acl_10

Rules are ordinary configuration: they live in `/mnt/data/config/local.conf`
with everything else `config set` writes, survive an upgrade and a rollback, and
can be read with `cat`, diffed between two switches and restored by copying a
file. The datapath re-reads them about once a second and reprograms the chip
when they change, so a rule takes effect without restarting anything and
without a link flapping.

The grammar:

    acl_<seq>=<permit|deny> [in <port>] [proto <name|number>]
                            [src <a.b.c.d[/len]>] [dst <a.b.c.d[/len]>]
                            [sport <n>] [dport <n>]

| word | matches | notes |
|---|---|---|
| `in <port>` | the port the packet arrived on | a tap name such as `swp6`; anything else is refused |
| `proto <p>` | the IP protocol | `icmp`, `igmp`, `tcp`, `udp`, `gre`, `esp`, `ah`, `ospf`, `vrrp`, `sctp`, or a number |
| `src <cidr>` | the source address | `/32` when the length is omitted |
| `dst <cidr>` | the destination address | likewise |
| `sport <n>` / `dport <n>` | the L4 port | only with `proto tcp` or `proto udp`, because the chip reads the port fields out of whatever follows the IP header |

Every word is optional; a rule with none of them matches every IPv4 packet.
`<seq>` is 1 to 999999.

## What happens to a packet

- Rules are evaluated **lowest sequence number first**, and the **first match
  decides**. A packet that matches no rule is forwarded as if there were no
  access list: rules narrow what the switch does, they do not replace it.
- **deny** drops the packet in the ingress pipeline, before forwarding and
  before the CPU. It is not punted either -- a denied OSPF hello never reaches
  `ospfd`, which is how a deny on the protocol takes an adjacency down.
- **permit** does nothing to the packet except stop a later `deny` from
  applying to it. That is what makes an exception: a permit at 5 above a deny
  at 10 lets through what the deny would have dropped.
- **Every rule counts** what it matched, whichever way it decided, because "the
  rule is in the chip and matched nothing" and "the rule is in the chip and
  matched the wrong thing" are different faults and only a counter tells them
  apart.

The rules are ingress only, IPv4 only, and match at layer 3 and 4. There is no
MAC matching, no port range, no rate limiting and no egress list yet; see the
limits below.

## Reading it back

    $ nosaic show acl
    SEQ  ACTION  MATCH                                    PACKETS  STATUS
    5    permit  in swp6 proto icmp src 10.101.101.26/32  10       in chip
    10   deny    in swp6 proto icmp src 10.101.101.26/32  0        in chip
    30   deny    dport 22                                 0        sport/dport need proto tcp or udp
    31   deny    in swp99                                 0        'swp99' is not a port on this switch

`PACKETS` is the chip's own hit counter. `STATUS` is either `in chip` or the
reason the rule is not: a rule that does not parse, names a port the switch
does not have, or that the chip refused is **shown with its error rather than
dropped silently**, and every other rule still installs.

`nosaic show caps` reports `acl yes, N rules`, where N is how many entries the
chip's field group can hold, or `acl no` on a datapath that has none -- in
which case every rule shows `no field group on this chip` and nothing is
filtered.

## Changing rules while running

A change is applied by taking every rule out of the chip and installing the new
set. That is **not atomic**: for the milliseconds in between, no rule is in
force. Cumulus avoids that window by keeping half the TCAM as a standby copy
and switching over, at the cost of half the capacity. NOSaic does not do that
yet; on a lab switch the window has not mattered, and on a switch where it
would, this is the thing to build next.

## Limits, stated

| | |
|---|---|
| Direction | ingress only |
| Address family | IPv4 only. IPv6 needs a second field group, because a 128-bit address does not share a key with a 32-bit one |
| Capacity | one field group; **1792 rules** on the AS5610 (Trident+), reported by `show caps` |
| Layer 2 | none: no MAC, VLAN or EtherType match |
| Ranges | none: one L4 port per word |
| Actions | permit, deny. No rate limit, no mirror, no class |
| Ordering | by sequence number, first match wins -- not iptables semantics, and `permit` is the only non-terminating-looking word that is in fact terminating |

## What was measured

On the AS5610-52X, 2026-09-16, with the box carrying its OSPF adjacencies
throughout:

- a `deny in swp6 proto icmp src <neighbour>` took 10 of 10 echo replies and
  the counter read 10; a `permit` of the same at a lower sequence let 10 of 10
  through, counted on the permit and not the deny;
- one counting `permit in <port> proto ospf` per neighbour counted only its own
  port's hellos (13, 9 and 10 in 45 seconds), and a `permit in swp6 ...` for a
  source that arrives on swp51 stayed at zero through 10 pings;
- `deny in swp6 proto ospf` took the swp6 adjacency down and left swp51 and
  swp52 Full; unsetting it brought swp6 back in 32 seconds;
- three malformed rules were reported with their reasons and the good rules
  installed around them.

## Where it is

`datapath/common/acl.c` is the whole thing: written against the SDK's
`bcm_field` API alone, so it is shared by every Broadcom datapath, and the
per-chip differences -- how many slices, which qualifiers fit one, single- or
double-wide -- are the SDK's decision and are reported rather than chosen.
Its file comment records the one decision that was not obvious: the ingress
port is qualified as a source port inside the key rather than through the
SDK's port bitmap, because on the AS5610 that bitmap reaches only one of the
chip's two ingress pipelines and a rule scoped to one port matched every other.
`nosaic show acl` is served by the `acl` query on the datapath socket, in both
CLIs.
