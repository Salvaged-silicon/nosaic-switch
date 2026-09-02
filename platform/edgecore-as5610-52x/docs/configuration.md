# What this switch is configured to do

Captured from the running EdgeNOS installation on 2026-09-02, so that NOSaic
can be brought up on this board doing the same job rather than a smaller one.
This is the record; the files that implement it are in
[`../config/`](../config/).

The switch is the lab's router. It carries the default route, runs OSPFv2 and
OSPFv3, and is the path several other boxes take to reach the build host — so
"NOSaic runs on it" and "NOSaic replaces it" are different bars, and this page
is the second one.

## Identity

| | |
|---|---|
| Hostname | `edgenos` today; `nosaic-as5610` in the FRR config |
| Router-id | `10.101.101.241`, the loopback |
| Management MAC | `80:a2:35:81:ca:ae`, from `board_eeprom` |

The front-panel MACs run consecutively from `80:a2:35:81:ca:af` (swp1) to
`80:a2:35:81:ca:e2` (swp52) — one per port, allocated in order after the
management port. Nothing in NOSaic reads `board_eeprom` yet, which is why
`network.conf` states the management MAC rather than deriving it.

## Addressing

Management is the P2020's own eTSEC behind a BCM54610C PHY, holding
`10.1.1.238/24` with a default via `10.1.1.1` — learned by DHCP on the running
box and pinned in configuration here, because a router that renumbers itself on
a lease change is one you cannot find again.

**It is `eth0` under NOSaic and `end0` under EdgeNOS.** Same hardware, same
gianfar driver, different name: EdgeNOS runs systemd, whose predictable naming
renames a device-tree NIC to `end0`; NOSaic runs s6 and nothing renames
anything, so the kernel's `eth0` stands. Transcribing the name from `ip addr`
on the running switch is therefore wrong, and it fails quietly — the interface
is never configured and is reported absent next to the front-panel ports that
genuinely are missing. Found exactly that way on the first configured boot.

Ten front-panel ports are routed, all at **MTU 1600** to match their OSPF
neighbours — a mismatch is an adjacency that reaches ExStart and stops. The
unaddressed ports sit at 9000.

| Port | Address | Role |
|---|---|---|
| swp49 | `10.101.101.18/29` | 40G uplink to the Cisco Nexus; carries the default route |
| swp51 | `10.101.101.66/29` | |
| swp52 | `10.101.101.49/29` | |
| swp1 | `10.101.101.1/29` | |
| swp2 | `10.101.101.10/29` | |
| swp4 | `10.101.102.1/29` | |
| swp5 | `10.99.99.1/24` | link-down on the running switch |
| swp6 | `10.101.101.25/29` | |
| swp7 | `10.101.101.33/29` | addressed, **not** in OSPF |
| swp8 | `10.101.101.41/29` | faces the Arista 7050SX2's et1 |

swp8 matters beyond its address: it is how the 7050SX2 reaches the build host
when that board's management port is down. Losing it costs the way back to a
different switch.

IPv6 runs on `lo` and swp1/2/4/6/7/8/49 in a globally routable /48. Those
addresses are **not** in this repository — they are the site's real assignment
rather than board data. They live in `config/network.site.conf`, which is
gitignored, and go on the switch as `/mnt/data/config/network.conf`, which the
network service reads in preference to the copy inside the image.

## Routing

OSPFv2 and OSPFv3, single area `0.0.0.0`, router-id `10.101.101.241`.

Costs are **10** on swp49 and swp52 and **40** on swp1, swp2, swp4, swp6 and
swp8, so the 40G path to the Nexus wins outright rather than on a tie-break.

Two ports are addressed and deliberately outside OSPF: swp5 (`10.99.99.0/24`)
and swp7 (`10.101.101.32/29`). Neither is covered by a `network` statement on
the running switch. That is reproduced rather than tidied up: if it is wrong it
is wrong on the router today, and this file should not quietly disagree with
the thing it is a record of.

`lo` is passive, so the router-id is advertised without the loopback trying to
form an adjacency with itself.

## Applying it

`network.conf` and `frr.conf` ship inside the image. The switch prefers
`/mnt/data/config/` for both, which is how one switch gets its own addressing
without that addressing passing through a public repository:

```sh
scp config/network.site.conf  <switch>:/mnt/data/config/network.conf
scp config/frr.conf           <switch>:/mnt/data/config/frr.conf
```

## What is not reproducible yet

**None of the swp ports exist.** They are created by the datapath daemon, and
`nosd-tdp` does not exist — so on this board today `network.conf` configures
`end0` and `lo` and reports the rest absent. `net_wait_secs` in `board.yml` is
short for exactly that reason and should go back to the default once there is a
datapath to wait for.

Until then NOSaic on this board is a control plane with no forwarding: it
boots, routes on its management port, and cannot replace what EdgeNOS is doing.
