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

## Spikes

A spike is a timeboxed investigation scoped by a question, not by a deliverable. It ends
when the question is answered — and "we could not answer this cheaply" is itself an answer
worth having early.

| | Question | Gate |
|---|---|---|
| **S1** | Can a current toolchain produce working binaries for 32-bit big-endian PowerPC (Freescale e500v2)? | A binary from that toolchain runs and reports the correct word size and endianness |

S1 runs ahead of M8 rather than as part of it. If the answer is no, that class of hardware
needs a pinned ancient compiler, and M8's scope changes — which is worth knowing before a
distro is built on the assumption that every architecture is equally reachable.

## Current state

**M0** complete. **M1** in progress: crosstool-NG pinned and building, defconfigs committed
and validated for x86_64, aarch64 and powerpc.
