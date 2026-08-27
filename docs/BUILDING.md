# Building NOSaic and running it in a VM

Everything here is the sequence CI runs on every commit, in the same order. If
a step fails for you and passes there, that is a bug worth reporting — the
container exists so that your machine and the runner are the same machine.

## What you need

**Docker and make.** Nothing else: the compiler, QEMU and every build tool live
in a pinned container, so nothing is installed on your host and nothing on your
host is used.

`NATIVE=1` on any target uses host tools instead. It is faster for iterating and
is *not* what CI does, so a NATIVE pass is weaker evidence than a container one.

## What it costs

Building a cross-toolchain and a libc from source is not quick, and it is
honest to say so before you start rather than after.

These are **measured from CI**, not estimates — a 4-core runner with `JOBS=4`.
They scale roughly with `JOBS`, so a slower machine or a lower job count will
take longer:

| Step | Measured | Notes |
|---|---|---|
| `make builder` | 1 min | Once, then cached |
| `make toolchain ARCH=x86_64` | **44 min** | Once. By far the longest step |
| `make packages PROFILE=minimal` | 14 min | glibc dominates |
| `make pkg PKG=linux` | 13 min | The kernel |
| `make image BOARD=virt-x86_64` | 26 s | |
| `make image-boot BOARD=virt-x86_64` | 20 s | Boots twice, on purpose |
| `make image-ab BOARD=virt-x86_64` | 33 s | |

So: **about 75 minutes from a clean clone to a VM you can log into**, most of it
the toolchain, which you build once.

The pinned source archives are about 490 MB. Total disk for a full build is
several gigabytes on top of that — the toolchain's build tree is the bulk of it,
and that figure has not been measured precisely, so leave headroom.

The build is bounded by limits derived from your host and shown by `make help`;
override them in `local.mk` (untracked):

```make
CPUS   = 8
MEMORY = 16g
JOBS   = 8
```

Those limits are enforced, not advisory. Raising them raises the load — an
earlier version of this build defaulted to every core and drove the host load
average to 70.

## The sequence

```sh
git clone https://github.com/salvaged-silicon/nosaic-switch
cd nosaic-switch

make check                          # validate the repo — seconds, do this first
make builder                        # the pinned build container

make toolchain ARCH=x86_64          # the long one
make toolchain-test ARCH=x86_64     # prove it makes binaries that actually run

make packages ARCH=x86_64 PROFILE=minimal
make pkg PKG=linux ARCH=x86_64

make image BOARD=virt-x86_64        # compose the A/B disk image
```

`make help` lists every target. `nosaic build` with no board lists the boards.

## Running it

Two ways, and they matter for different reasons.

**As a test** — boots, self-tests from inside the running system, and powers off:

```sh
make image-boot BOARD=virt-x86_64
```

It boots *twice*. The second boot is the point: it proves the data partition is
real, because a tmpfs pretending to be persistent passes everything a single
boot can check.

**As a machine you can use** — a console on this terminal, no self-test, no timeout:

```sh
make vm BOARD=virt-x86_64
```

Log in as **`admin`**. There is **no password**: NOSaic ships without one, so
that no image is ever reachable using a credential its author already knows.
Set one with `passwd` — it lands on the data partition, so it survives upgrades
and rollbacks. Until it is set, the account works on the console only and SSH
refuses password authentication.

Leave QEMU with **Ctrl-a x**; shut down cleanly with `poweroff`.

Both paths build their QEMU command line from the same code (`boot/virt/qemulib.sh`),
so the VM you get is the one CI tested rather than a lookalike.

## Proving the parts you care about

```sh
make image-ab BOARD=virt-x86_64   # A/B upgrade commits, and rolls back when broken
make dataplane-test               # drive a real veth datapath through switch-api
make kernel-boot ARCH=x86_64      # boot the kernel alone with an init we built
```

`image-ab` tests both directions deliberately. A happy-path-only upgrade test is
passed perfectly by an implementation that always commits, and that
implementation has no safety net at all.

## When something fails

- **`make check` fails on a clean clone.** That is a real bug; nothing else is
  worth trying first.
- **A package fails to configure.** Cross-compilation usually fails by finding
  the *wrong* thing rather than nothing, so the error often points somewhere
  unrelated to the cause. Check whether the build picked up a host library.
- **The image boots but the self-test fails.** The console log is kept at
  `out/images/<board>/console.log`, and the second boot at `console-2.log`.
- **QEMU exits immediately.** Usually the image is missing — `make image` first.

## What is not here yet

The only board is `virt-x86_64`, which is a real image with a virtual dataplane
(veth pairs behind a bridge) rather than a stub. Real switches land from M6; see
[MILESTONES.md](MILESTONES.md). The `full` and `slim` profiles are unfinished —
`minimal` is the one that boots today.
