# IS-IS

FRR's `isisd` runs on every switch, beside `ospfd` and `ospf6d`. It is
configured the same way they are: in frr.conf, or live through `vtysh`. With
no `router isis` in the configuration it runs idle.

It needs nothing from the datapath. IS-IS PDUs are not IP: they are LLC frames
to IS-IS's own multicast addresses (`01:80:c2:00:00:14`, `01:80:c2:00:00:15`,
and `09:00:2b:00:00:05` on point-to-point links). The Broadcom datapaths
already deliver those to the port's tap, and `isisd` speaks on the tap like
any Linux interface. The routes it learns reach the chip the way every
other route does, through the kernel and the route mirror, so `nosaic verify
routes` covers them.

## Configuration

In the switch's own `/mnt/data/config/frr.conf`, like OSPF:

    router isis CORE
     net 49.0001.0101.0125.5053.00
     is-type level-2-only
    !
    interface et3
     ip router isis CORE
     ipv6 router isis CORE
     isis network point-to-point
    !
    interface lo
     ip router isis CORE
     isis passive

- The NET's system ID is conventionally the loopback address, padded to
  twelve digits: `10.101.255.53` is `0101.0125.5053`.
- `isis network point-to-point` on a link between two switches skips the DIS
  election and its pseudonode.
- ⚠ frr.conf is copied into `/etc/frr` at boot. A change to
  `/mnt/data/config/frr.conf` takes effect on the next boot, or copy it and
  restart `isisd` (`s6-svc -r /run/service/isisd`).

Where OSPF runs as well, OSPF's routes win: FRR's administrative distance
is 110 for OSPF and 115 for IS-IS.

## Proven

On 2026-09-28, with each switch's OSPF left running:

- **Adjacencies** formed, level 2, point-to-point:
  - the 7050SX2 (Trident2+) with the AS5610 (Trident+), et3 to swp1;
  - the 7050SX2 with the 7050TX-64 (Trident2), et52 to et49.
- **Routes:** a prefix only IS-IS knew, on the AS5610's loopback, was learned
  by the SX2 and, two IS-IS hops away, by the TX. It was in each switch's chip
  (`verify routes`: present).
- **Forwarding:** 20 of 20 pings from the TX were routed through the SX2 on the
  IS-IS route. With the TX's own IS-IS route in charge, 10 of 10.

## Not yet

- No IS-IS-specific datapath trap. The PDUs reach the CPU because the chip
  floods unknown multicast to the CPU on a routed port. An access list that
  denied that traffic would stop IS-IS without saying so.
- Not run on the Nexus 3172TQ. It has the TX's datapath and chip family.
