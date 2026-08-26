# Milestones

Each milestone has a gate. Nothing counts as done without it, and every milestone
leaves something bootable.

| | Milestone | Gate |
|---|---|---|
| **M0** | A clean clone builds | Clone into an empty directory on a machine with only Docker and make; `make check` passes |
| **M1** | Toolchains | crosstool-NG produces x86_64 and aarch64 toolchains; each compiles a binary that runs |
| **M2** | Recipe engine and `.nos` packages | A leaf package builds in CI; dependencies install in topological order; two builds produce identical hashes |
| **M3** | Base system, and a VM that boots | `nosaic build virt-x86_64` boots under QEMU in CI for all three profiles; the CLI configures a port, VLAN, address and route; an A/B upgrade, trial boot, commit and auto-rollback all pass |
| **M4** | One kernel | Boots on x86_64 under QEMU; cross-builds clean for aarch64 |
| **M5** | The boot axis | Two bootloader backends emit installable images; a corrupted image is rejected rather than installed |
| **M6** | First real board | Boots from its own from-source base on real hardware, reports real sensors, forwards traffic — and the M3 CLI test passes unmodified |
| **M7** | Routing and upgrades | BGP establishes; an upgrade that boots but fails to forward rolls back unattended |
| **M8** | Older architectures | PowerPC and armhf toolchains, `uboot` and `onl-swi` backends, and ports for older boards |

## Current state

**M0.** The skeleton, the CLI, and the checks that enforce the design's invariants.
