# VXLAN & BGP EVPN (L2 and L3)

NOSaic supports VXLAN overlays driven by FRR's BGP EVPN control plane
(`address-family l2vpn evpn`):

- **L2 EVPN (Type-2 MAC + Type-3 IMET)**: stretches VLANs across an L3 IP
  fabric over VXLAN (UDP port 4789).
- **L3 EVPN (Type-5 IP Prefix)**: routes isolated tenant VRFs over L3 VNIs.

## Silicon Support

| ASIC | L2 VXLAN | L3 VXLAN |
|---|---|---|
| Trident II (`td2`, e.g. Dell S6000-ON) | Yes (line rate) | Yes (VRF-aware Type-5) |
| Trident II+ (`td2p`, e.g. Arista 7050SX2-72Q) | Yes (line rate) | Yes (single-pass RIOT) |
| Trident+ (`tdp`, e.g. Edgecore AS5610-52X) | No (returns unsupported) | No |

Kernel support (`CONFIG_BRIDGE=y`, `CONFIG_VLAN_8021Q=y`, `CONFIG_VXLAN=y`,
`CONFIG_NET_VRF=y`) is built into `recipes/linux/config/common.fragment`.

## CLI

```sh
nosaic vxlan vtep <ipv4>                             # local VTEP source IP
nosaic vxlan vni add <vni> vlan <vid>                # L2 VNI <-> VLAN
nosaic vxlan vni add <vni> vrf <vrf> vlan <vid>      # L3 VNI <-> VRF + transit VLAN
nosaic vxlan vni del <vni>
nosaic show vxlan
```

All VXLAN directives are included in `nosaic config running`, `nosaic config diff`,
and `nosaic config save`.

## Persistent Configuration (`/mnt/data/config/network.conf`)

`apply-network.sh` programs `nosd` and creates the matching Linux `br<vid>` and
`vxlan<vni>` (`dstport 4789`, `nolearning`) interfaces (enslaved to the VRF for
L3 VNIs) so FRR `zebra`/`bgpd` discovers and advertises them automatically.

```
# Tenant VRF + local VTEP loopback IP
vrf tenant-a table 100
vxlan vtep 10.255.0.1

# L2 VNI (stretched VLAN 700)
vxlan vni 10700 vlan 700

# L3 VNI (tenant-a Type-5 routing, using internal transit VLAN 4001)
vxlan vni 50100 vrf tenant-a vlan 4001
```

## FRR BGP EVPN Example (`vtysh` / `/mnt/data/config/frr.conf`)

```
vrf tenant-a
 vni 50100
exit-vrf

router bgp 65001
 bgp router-id 10.255.0.1
 neighbor <remote-vtep> remote-as 65002
 address-family l2vpn evpn
  neighbor <remote-vtep> activate
  advertise-all-vni
 exit-address-family

router bgp 65001 vrf tenant-a
 address-family ipv4 unicast
  redistribute connected
 exit-address-family
 address-family l2vpn evpn
  advertise ipv4 unicast
 exit-address-family
```

## How the Datapath Syncs

- `datapath/common/vxlan.c` initializes `bcm_vxlan_init` (UDP 4789), creates
  hardware VXLAN VPNs and BUM multicast groups, binds front-panel VLAN members
  as local VXLAN access ports, and listens on `NETLINK_ROUTE` (`RTMGRP_NEIGH`,
  `AF_BRIDGE`) with a 30s reconciliation dump:
  - `00:00:00:00:00:00` → remote VTEP (EVPN Type-3 IMET) adds/removes the remote
    VTEP tunnel on the VNI's BUM multicast group.
  - Unicast MAC → remote VTEP (EVPN Type-2) installs/deletes `bcm_l2_addr_add`
    entries pointing at the remote VXLAN network gport.
- `datapath/common/l3sync.c` reads `RTA_TABLE`, maps L3 VNI VRF tables to
  hardware `bcm_vrf_t` IDs, and installs Type-5 VRF routes via
  `nosaic_vxlan_l3_egress()`.

## Verification

```sh
nosaic show vxlan
vtysh -c "show bgp l2vpn evpn"
vtysh -c "show evpn vni"
grep vxlan /var/log/nosd/current
```

*(Note: VXLAN adds 50 bytes of IPv4 overhead — ensure underlay links carry MTU ≥ 1550.)*
