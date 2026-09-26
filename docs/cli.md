# The nosaic CLI

The commands a running switch answers. There are two implementations of the
same vocabulary: the Go CLI on x86_64 and aarch64 boards, and a C CLI on the
AS5610's 32-bit big-endian PowerPC, which Go cannot target. Both ask the same
ops on the same socket, `/run/nosd.sock`, and print the same columns.
[DESIGN.md](DESIGN.md#the-same-commands-on-every-switch) explains why that
matters. Where the two differ, the table says so.

Commands that talk to the datapath need root: the socket is root-only. Log in
as `admin` and use `doas nosaic ...`; `sudo` is a shim onto it.

## The datapath

| command | | Go | C |
|---|---|---|---|
| `show ports` | admin and oper state, speed, MTU, per port | ✅ | ✅ |
| `show routes` | the chip's forwarding table | ✅ | ✅ |
| `show vlans` | VLANs, their SVI, untagged and tagged members | ✅ | ✅ |
| `show lags` | LAGs, their mode, and which members carry traffic | ✅ | ✅ |
| `show stp` | the spanning tree: bridge, root, and each port's role and state | ✅ | ✅ |
| `show mlag` | the MLAG pair: role, peer, heartbeat, and each MLAG interface | ✅ | ✅ |
| `show gateways` | the virtual gateways and their MAC | ✅ | ✅ |
| `show acl` | access-list rules and their hit counts | ✅ | ✅ |
| `show caps` | what this datapath can do, and the contract version | ✅ | ✅ |
| `show dma` | the SDK's DMA pool, by allocation name | ✅ | ✅ |
| `interface <port> up\|down` | administrative state | ✅ | — |
| `interface <port> mtu <n>` | MTU | ✅ | — |
| `route add <prefix> via <ip> dev <port> [...]` | a static route; repeat `via` for ECMP | ✅ | — |
| `route del <prefix>` | | ✅ | — |
| `verify contract` | the switchapi conformance suite against this datapath | ✅ | — |
| `verify ports` / `verify routes` | Linux against the chip, side by side | — | ✅ |

`interface <port> up|down` goes through the contract call `port.admin`, which
the Broadcom datapaths serve since switchapi 1.3. It enables or disables the
port in the chip, so the far end loses link, as it would if the cable were
pulled. `interface <port> mtu` and `route` go through calls those datapaths
do not serve yet. On those boards, addresses and routes are applied by
`apply-network.sh` from `network.conf` ([vrf.md](vrf.md) and
[vlan.md](vlan.md) for its grammar).

### Access lists

    acl add <seq> permit|deny [ipv4|ipv6] [in <port>] [proto <p>]
                              [src <prefix>] [dst <prefix>] [sport <n>] [dport <n>]
    acl del <seq>

Both CLIs. Persisted as the setting `acl_<seq>`. [acl.md](acl.md).

### VLANs and SVIs

    vlan add <vid>
    vlan del <vid>
    switchport <port> access <vid>
    switchport <port> trunk <vid>[,<vid>...] [native <vid>]
    switchport <port> none
    svi add <vid>
    svi del <vid>

Both CLIs. `switchport` states the port's whole membership. The same
statements go in `network.conf`. [vlan.md](vlan.md).

### LAGs

    lag <poN> lacp|static <port>[,<port>...]
    lag <poN> none

Both CLIs. `lag` states the LAG's whole membership. A LAG's name goes
wherever a port's does, `switchport` included. [lag.md](lag.md).

### Spanning tree

    stp on [priority <n>]
    stp off
    stp port <port> [edge] [cost <n>]

Both CLIs. Rapid spanning tree over the switched ports and LAGs; off until
turned on. [stp.md](stp.md).

### MLAG

    mlag on peer-link <port> [peer-address <ip>] [priority <n>]
    mlag off
    lag <poN> lacp|static <port,...> mlag <id>

Both CLIs. One of a pair of switches presenting a LAG with the same MLAG id as
one LACP partner. [mlag.md](mlag.md).

### Virtual gateway

    gateway add|del <svi> <address/len>
    gateway mac <mac>

Both CLIs. A gateway address both switches of a pair answer for and route
for. [gateway.md](gateway.md).

## Settings

| command | | Go | C |
|---|---|---|---|
| `config show [pattern]` | every setting, and whether it came from the image or this switch | ✅ | ✅ |
| `config get <name>` | one setting's effective value | ✅ | ✅ |
| `config set <name> <value>` | write it into this switch's own configuration | ✅ | ✅ |
| `config unset <name>` | remove it; the image's default applies again | ✅ | ✅ |
| `config files` | which files are layered, in order | ✅ | ✅ |

## Upgrades

| command | | Go | C |
|---|---|---|---|
| `upgrade status` | which slot is committed, and any trial in progress | ✅ | ✅ |
| `upgrade install <rootfs.sqsh> [--slot a\|b]` | into the inactive slot, marked for trial | ✅ | ✅ |
| `upgrade commit` | keep the slot on trial | ✅ | ✅ |
| `upgrade confirm` | check this image and commit it if it works | ✅ | ✅ |

A slot holds the root filesystem only. The kernel and initramfs are outside
A/B: the SWI on an Arista, the FIT on the AS5610.

## The board

`nosaic platform <command>`, the platform HAL: box hardware, not forwarding.
What a board supports depends on its HAL. `nosaic platform` with no argument
lists them. The Go CLI's set:

    status                  what the board reports about itself
    mac                     the board's own base MAC, from its identity PROM
    release-asic            take the switch chip out of reset
    asic                    what the switch chip says about itself
    transceivers            which cages have modules in them
    retimer [--program]     the signal repeater in front of some cages
    tx <cage|all> on|off    transmitters on or off
    thermal [--once] [--interval N]
                            the cooling loop
    beacon [on|off]         the locator LED
    linkmap                 which ports the chip will actually egress to
    i2c ...                 raw i2c, bring-up only
    schan selftest|read     S-Channel, bring-up only
    watchdog status|arm|disarm|raw

The C CLI on the AS5610 has `status`, `thermal`, `ledwalk`, `transceivers`
and `xcvr <cage> [raw]`, and names the rest as unsupported rather than
leaving them unknown.

## On a build host

`version`, `check`, `boards`, `board scaffold`, `pkg build|info|verify|order`,
`build <board>` and `docs index`. [BUILDING.md](BUILDING.md).
