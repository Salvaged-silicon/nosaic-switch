# Milestones

Each milestone has a gate. Nothing counts as done without it, and every milestone
leaves something bootable.

| | Milestone | Gate |
|---|---|---|
| **M0** | A clean clone builds | Clone into an empty directory on a machine with only Docker and make; `make check` passes |
| **M1** | Toolchains | **Done.** crosstool-NG 1.28.0 produces x86_64, aarch64 and powerpc toolchains from committed defconfigs; each compiles a binary that runs and passes three gates |
| **M2** | Recipe engine and `.nos` packages | **Done.** zlib builds from source for x86_64 and powerpc; two clean builds are byte-identical; dependencies resolve in topological order; ELF objects are verified against the target |
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
| **S1** | Can a current toolchain produce working binaries for 32-bit big-endian PowerPC (Freescale e500v2)? | **Answered: yes.** gcc-15.2.0 / glibc-2.42, soft-float, built in 47 min; the binary ran and reported `bits=32 endian=big`, and an audit of all 112,775 instructions found no FPU, SPE or AltiVec instruction anywhere. Hardware confirmation on a real board outstanding. |

S1 runs ahead of M8 rather than as part of it. If the answer is no, that class of hardware
needs a pinned ancient compiler, and M8's scope changes — which is worth knowing before a
distro is built on the assumption that every architecture is equally reachable.

## Current state

**M0**, **M1** and **M2** complete. **S1** answered.

Three toolchains, each gated on three independent properties rather than "did it build":

| | triple | runs | instruction audit | ABI floor |
|---|---|---|---|---|
| x86_64 | `x86_64-nosaic-linux-gnu` | 64-bit LE | — | Linux 3.2.0 |
| aarch64 | `aarch64-nosaic-linux-gnu` | 64-bit LE, under QEMU | — | Linux 3.7.0 |
| powerpc | `powerpc-nosaic-linux-gnu` | **32-bit BE**, under QEMU | 0 forbidden of 112,775 | Linux 3.2.0 |

Each gate exists because something went wrong without it. The ABI floor check
was added after two toolchains silently pinned themselves to Linux 6.16.0,
which would have refused to start on the very boards this project targets. The
instruction audit was added because an emulator is permissive enough to run
instructions the real CPU traps on.

### M2

The first package built from source is `zlib` — chosen over the originally
planned `json-c` because json-c moved to CMake, which would have meant
implementing a second build system before the first package existed. zlib has
no dependencies, is needed by nearly everything, and its build is plain enough
that a failure is a failure of our machinery rather than of the package.

The same recipe, built for two architectures, produces genuinely different
machine code:

```
x86_64:  ELF 64-bit LSB shared object, x86-64
powerpc: ELF 32-bit MSB shared object, PowerPC
```

That is checked mechanically now, not by eye: every ELF object in a package is
compared against the target's machine, word size and endianness before the
package is written. A cross-build that silently used the host compiler
otherwise produces a package that builds, installs, and fails only on the
switch.

Next: **M3**, the base system and a VM that boots.
