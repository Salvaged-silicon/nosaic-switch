# The management VRF

The management port in a routing table of its own, so its routes are never
mixed with the ones the front panel routes by. This is what Cisco calls
`vrf management` and Cumulus calls `vrf mgmt`. Running on every NOSaic switch
in the lab since 2026-09-24: the 7050SX2-72Q, 7050TX-64, AS5610-52X and the
Nexus 3172TQ.

## Why

With eth0 in the main table, a management port shares one routing table with
the front panel, and two things go wrong:

- **The switch's own replies to its management station** follow whatever the
  front panel learned. On the 7050SX2, OSPF learned the build network over a
  front-panel link, and a learned /24 beats the default route. Its ssh and
  file transfers went out of the front panel and back through the CPU punt
  path, at 21 KB/s. The fix was a static route pinning the build network to
  eth0: it named one network, and any other prefix the switch learned could
  do the same again.
- **A packet transiting the box that missed in the chip** was routed by the
  kernel with eth0's routes in view.

The main table is also the one the datapath mirrors into the chip. It never
programmed eth0's routes, because eth0 has no router interface, but keeping
eth0 out of that table altogether is the clean line.

## Configuration

In `network.conf`:

    vrf mgmt table 1001
    iface eth0 10.10.32.2/24 mac 44:4c:a8:eb:93:f6 vrf mgmt
    iface eth0 2001:470:882d:32::2/64 vrf mgmt
    route default via 10.10.32.1 vrf mgmt
    route default via 2001:470:882d:32::1 vrf mgmt

- `vrf <name> table <N>` creates the VRF. Every route line that names the VRF
  is translated to `table N`, because busybox's `ip`, which is what a minimal
  image has, has no `vrf` keyword for routes.
- `vrf <name>` on an `iface` line puts the interface in it. This happens
  before the address, and only when it is not there already. Joining a VRF
  cycles the link, which throws away a static IPv6 address, and a reconcile
  pass that re-joined each time would flap the management port every 30 s.
- `vrf <name>` on a `route` line puts the route in the VRF's table.
- The static "pin" route that the example files used to carry is gone: the
  VRF replaces it.

`apply-network.sh` also sets `tcp_l3mdev_accept=1` and `udp_l3mdev_accept=1`
when any VRF is configured. Services listen in the default VRF: dropbear has
no option to bind to a VRF, and busybox has no `ip vrf exec`. Without those
sysctls, ssh would answer on the front panel and not on the management port.

## Using it

From an operator's side, nothing changes: ssh to the management address
works as it did, and on the 7050SX2 a 20 MB pull over it ran at the pinned
rate.

⚠ **From the switch's own shell, the management network is in another
table.** The default VRF has no route to it at all:

    ping 10.22.1.5                  Network is unreachable
    ping -I mgmt 10.22.1.5          answers

Anything the switch itself originates towards the management side needs
`-I mgmt` or an equivalent SO_BINDTODEVICE.

## The kernel

`CONFIG_NET_VRF`, `CONFIG_NET_L3_MASTER_DEV` and
`CONFIG_IPV6_MULTIPLE_TABLES` are in `recipes/linux/config/common.fragment`,
built in, along with `IP_MULTIPLE_TABLES`, which is asked for rather than
inherited.

⚠ **The kernel is outside the A/B slot.** An image upgrade that brings a
`vrf` line does not bring a kernel that understands it. The SWI (Aristas),
the FIT partition (AS5610) or the netboot image (3172TQ) has to be updated
too, or the line fails with `vrf mgmt table N FAILED` and eth0 stays where
it was. Every lab switch was reflashed on 2026-09-24. The previous kernels
are kept:
- `nosaic-ab-prevrf.swi` on the 7050SX2's flash;
- `nosaic-prevrf.swi` on the 7050TX-64's;
- the AS5610's old FIT, backed up off the box.

## What it does not do

- **Only eth0 can be in a VRF.** VRFs on front-panel ports would need the
  datapath to program per-VRF tables into the chip. It does not: l3sync reads
  the main table only for IPv4. For IPv6 it relies on the interface test,
  because `/proc/net/ipv6_route` has no table column.
- **No FRR in the VRF.** FRR sees the VRF (`show vrf`) but runs its routing
  in the default one.
- **A line removed from the file is not undone.** Taking eth0 back out of the
  VRF is `ip link set eth0 nomaster` and restoring the routes by hand.
