# virt-x86_64 — the virtual platform

Board number one, and not a test fixture. It is what keeps `main` provable with
no switch attached, and it is where the `switch-api` contract was defined, so
that no real ASIC could bias it. It stays permanently green in CI: if it breaks,
the core has grown a dependency on hardware it is not supposed to have.

| | |
|---|---|
| Arch | x86_64 |
| ASIC | `virt` — veth pairs behind a Linux bridge |
| Boot | `virt` — QEMU is given the kernel, initramfs and disk directly |
| Profile | minimal |
| Status | bringup |

- **[Install](docs/install.md)** — run it under QEMU
- **[Build](docs/build.md)** — build an image for it
- **[Hardware reference](docs/hardware.md)** — topology, disk layout, boot chain

## What it does

Contract 1.13, with the Linux kernel standing in for the chip:

- **VLANs and SVIs** — a VLAN-filtering bridge
- **LAG**, static and LACP — bonds
- **Spanning tree** — the bridge's own, which is 802.1D rather than rapid
- **BPDU guard and root guard** — the bridge port's `guard` and `root_block`
- **MAC aging** — the bridge's `ageing_time`
- **Access lists**, IPv4 and IPv6 — nftables
- **Routes, ECMP and IPv6** — the kernel's routing table

**Not supported, by design:** MLAG (a Linux bond cannot present one LACP system
from two machines), the virtual gateway, and QinQ (a Linux bridge is 802.1Q or
802.1ad for every port at once, never both side by side). Each is refused as
unsupported rather than imitated. IS-IS has not been tested here. The details
are in [hardware.md](docs/hardware.md#datapath).

## Reverse engineering

None. There is no silicon to reverse engineer, which is the point: the contract
this board defines had to come from somewhere with no vendor to imitate.
