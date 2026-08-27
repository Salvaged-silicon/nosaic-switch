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

## Reverse engineering

None. There is no silicon to reverse engineer, which is the point: the contract
this board defines had to come from somewhere with no vendor to imitate.
