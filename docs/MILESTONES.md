# Milestones

Each milestone has a gate. Nothing counts as done without it, and every milestone
leaves something bootable.

| | Milestone | Gate |
|---|---|---|
| **M0** | A clean clone builds | Clone into an empty directory on a machine with only Docker and make; `make check` passes |
| **M1** | Toolchains | **Done.** crosstool-NG 1.28.0 produces x86_64, aarch64 and powerpc toolchains from committed defconfigs; each compiles a binary that runs and passes three gates |
| **M2** | Recipe engine and `.nos` packages | **Done.** zlib builds from source for x86_64 and powerpc; two clean builds are byte-identical; dependencies resolve in topological order; ELF objects are verified against the target |
| **M3** | One kernel | **Done.** 6.12 LTS boots under QEMU on x86_64 *and* aarch64, running an init built by its own toolchain that verifies the configured filesystems are present |
| **M4** | Base system, and a VM that boots | **Done.** Boots, persists, upgrades atomically with rollback, and the CLI drives a real veth datapath through the contract. All three profiles build and boot in CI: minimal on busybox+s6, slim and full on systemd |
| **M5** | The boot axis | **Mostly done.** Four backends (virt, onie-sfx, aboot, uboot) emit installable artifacts, tested by running the installer's own extraction and by reading the FIT back. Aboot needs confirming on hardware |
| **M6** | First real board | Boots from its own from-source base on real hardware, reports real sensors, forwards traffic — and the M3 CLI test passes unmodified |
| **M7** | Routing and upgrades | BGP establishes; an upgrade that boots but fails to forward rolls back unattended |
| **M8** | Older architectures | PowerPC and armhf toolchains, the `onl-swi` backend, and ports for older boards |

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

**M0** through **M3** complete. **S1** answered.

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

### The kernel comes before the base

These two were originally the other way round, which was wrong: the base
milestone's gate is "boots under QEMU", and nothing boots without a kernel.
The alternative — borrowing a distribution kernel to prove the base, then
replacing it — would have meant building a throwaway boot path that proves less
than it appears to, since a distribution kernel brings module and initramfs
assumptions a from-source base does not reproduce.

The kernel has no dependency on the base, so it simply goes first.

### M3

Linux 6.12 LTS, one version fleet-wide, built as a recipe like anything else
rather than as a special case with its own script.

Configuration is a 45-line common fragment plus six or seven architecture-specific
lines — not a committed 5,000-line `.config` that nobody reviews and that rots
across versions. Every symbol is verified after `olddefconfig`, because appending
to `.config` is only a request: a symbol whose dependencies are unmet is dropped
in silence, and the symptom arrives much later as an image that cannot mount its
own root.

That check earned itself immediately. On aarch64 it caught `CONFIG_BRIDGE`
coming out as a module where the fragment asked for built-in — on the
architecture nobody is testing by hand. It also forced a distinction worth
keeping:

| | Why |
|---|---|
| **`=y` required** — squashfs, overlayfs, ext4, virtio-blk, console | needed to mount the root; a module cannot load from a filesystem that is not mounted yet |
| **`=m` accepted** — veth, bridge, VLAN | nothing needs them until userspace is running |

So built-in satisfies a request for a module, never the reverse.

The boot test does not merely check that a kernel starts. It builds an init with
that architecture's own toolchain, boots it under QEMU, and has it report from
inside the running kernel which filesystems are actually present — so a kernel
that boots but is missing something it was configured with fails, rather than
passing and disappointing someone later.

### M4, so far

NOSaic boots.

```
NOSAIC-INITRAMFS image mounted
NOSAIC-INITRAMFS overlay assembled
NOSAIC-BOOT userspace reached
nosaic login:
NOSAIC-SELFTEST OK
```

An image is composed from packages, never assembled by hand: the board names a
profile, the profile names packages, and their closure is the image. What is in
one can be answered by reading `/etc/nosaic/image.json` on the box rather than
by inspecting a filesystem.

The boot test does not stop at a login prompt. Reaching one proves the boot
path but not that the system is usable, so a self-test runs alongside `getty`
and checks the things an image must actually have — a writable overlay, its own
identity, the login account with no password — then powers off. Running it
*alongside* getty rather than before matters: powering off first would make
"a login prompt appeared" and "the self-test passed" mutually exclusive, which
is a mistake this gate made once already.

### A/B upgrades

    3. trial boot   NOSAIC-BOOT-TRIAL slot b attempt 1 of 3
                    NOSAIC-SELFTEST COMMIT the trial slot is now active
    5. trial boot   NOSAIC-BOOT-ROLLBACK slot a does not contain a mountable
                    image; returning to b

Both directions are tested, because a happy-path-only test is passed perfectly
by an implementation that always commits — and that implementation has no
safety net at all. So the suite installs 4 MB of `/dev/urandom` into a slot and
requires the switch to come back on the known-good one, and it requires an
attempt to overwrite the *running* slot to be refused, since an installer that
permits that has quietly deleted the thing you would roll back to.

The commit is gated on the health checks rather than on having reached
userspace. An image that boots and does not work is precisely the case rollback
exists for, so committing because init ran would defeat the mechanism.

Two failure modes are handled differently on purpose. A trial slot that will
not mount rolls back immediately — there is nothing to learn from retrying
garbage three times. A trial slot that mounts but never confirms burns a retry
budget first, because "booted but unhealthy" may be transient.

### What A/B does not cover: the kernel and the initramfs

A slot holds a root filesystem and nothing else. The kernel and the initramfs
live where the bootloader can reach them without understanding our layout -- in
the SWI on an Arista, in a raw FIT partition on the AS5610 -- and there is one
of them, not two.

So installing a slot does not change the kernel or the initramfs, and a change
to either is **outside the rollback mechanism**: it is written in place, and a
bad one is recovered by catching the bootloader, not by a trial expiring. This
is not an oversight but it is a real limit, and it cost a boot to learn: a
change to the initramfs was built, the slot was installed and trialled, and the
new behaviour was simply absent because the SWI had never been pushed.

Two consequences worth stating. An upgrade that spans both -- a new kernel and
a new root filesystem -- is only half atomic. And anything the confirmation
depends on must live in the slot, not the initramfs, or a rollback cannot
restore it.

### Confirming a trial on real hardware

The commit above is `NOSAIC-SELFTEST COMMIT`, and for a long time that was the
only thing that ever committed one. It could not run on a switch: `selftest.sh`
begins `grep -q nosaic.selftest /proc/cmdline || exit 0`, and that flag is set
by this harness and by nothing else. So on hardware a *good* upgrade rolled
back exactly like a bad one, three boots later, with nothing saying why. The
mechanism looked complete and was inert everywhere it mattered.

Hardware now runs a `trial-confirm` service on every boot. With no trial
pending it reads the pointer and exits. With one, it waits for the datapath --
generously, because a daemon carrying a vendor SDK takes about 80 seconds to
initialise a Trident2+ and a check that races it would roll back a perfectly
good image -- and then asks whether this image actually works: the datapath
must answer, know about ports, and if anything is configured up, something must
actually be up. `nosaic upgrade commit` is the explicit form for an operator
who has decided already.

It also detaches, which is a requirement rather than tidiness. The service is
an s6 oneshot and `s6-rc -u change default` waits for every oneshot with a
120-second budget, while confirmation legitimately takes longer: it allows five
minutes for a datapath, because a daemon carrying a vendor SDK needs about
eighty seconds to bring up a Trident2+ and a check that races it would roll back
a good image. Run inline, a trial whose datapath never comes up would hold the
bundle past its deadline, the init script would read that as the service
database failing, and a switch that was merely *declining an upgrade* would drop
to a rescue shell and look far worse than it is. Nothing downstream depends on
the answer — the rollback is driven by the initramfs counter, not by this
process finishing — so the boot does not wait for it.

That service is deliberately **not** ordered after `nosd`. s6 will not start a
service whose dependency never comes up, and a datapath that never comes up is
the exact failure this exists to report -- ordering it after `nosd` made the
most important case the silent one. It waits for the datapath itself instead.

The slot pointer lives on the boot partition, which is deliberately ext2 with
no journal. That is not a detail: it was first put on the ext4 data partition,
where `debugfs` wrote it, `upgrade status` read it back correctly, and the
kernel then reverted it by replaying the journal on mount. The installer
reported success and the switch booted the old slot anyway.

### The datapath

    driver  virt        vlans  false       ecmp  yes, up to 32 paths

    PREFIX           NEXT-HOPS
    192.0.2.0/24     10.0.0.2 dev swp1
    198.51.100.0/24  10.0.0.2 dev swp1, 10.0.1.2 dev swp2

`nosd` owns the datapath; everything else configures it over a Unix socket. The
client implements the same `switchapi.Switch` interface as the server, so the
conformance suite runs *through* the socket — which caught the thing most
likely to be lost in serialisation, and most damaging to lose: the difference
between "this hardware cannot" and "this went wrong".

The virtual datapath is not a stub. Ports are veth pairs carrying real packets,
addresses and routes are installed in the kernel, and multipath is the kernel's
own — so the ECMP path of the contract is exercised against something that
genuinely implements it.

It declares VLANs unsupported rather than faking them. Doing them properly
means a bridge with VLAN filtering, which changes how addresses behave on a
port; half-implementing them would be exactly what the conformance suite exists
to catch.

Still to come in M4: the systemd profiles.

Next: the full and slim profiles.
