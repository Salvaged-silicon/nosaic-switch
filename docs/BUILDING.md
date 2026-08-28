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

Both columns are **measured, not estimated**. CI is a 4-core runner with
`JOBS=4` and Docker's `overlay2` driver; "a laptop" is a 6-core machine sharing
those cores with a desktop, running Docker on the much slower `vfs` driver, so
treat it as a worst case rather than a target:

| Step | On CI | On a laptop |
|---|---|---|
| `make check` | — | 2 min |
| `make builder` | 1 min | 2 min |
| `make toolchain ARCH=x86_64` | **44 min** | **51 min** |
| `make toolchain-test` | — | 39 s |
| `make packages PROFILE=minimal` | 14 min | 33 min |
| `make pkg PKG=linux` | 13 min | 15 min |
| `make image BOARD=virt-x86_64` | 26 s | 76 s |
| `make image-boot BOARD=virt-x86_64` | 20 s | 86 s |
| `make image-ab BOARD=virt-x86_64` | 33 s | 2 min |

So: **roughly 75 minutes on CI, under two hours on a busy laptop**, most of it
the toolchain, which you build once.

### Disk

This is the part worth reading before you start, because the build needs an
order of magnitude more space than it keeps:

| | |
|---|---|
| **Peak, during the build** | **~18 GB** |
| What survives it | ~1.4 GB — toolchain 435 MB, sources 487 MB, output 444 MB |

The difference is intermediate build trees under `.cache/`, and the toolchain's
alone is 14 GB. Keeping them makes a rebuild much faster, but if you are short
of space, that is where it went:

```sh
make clean              # build output and the package trees
make clean-toolchains   # the toolchains and their build trees
```

Sources in `dl/` are kept by both, so a rebuild needs no network.

### Limits

The build is bounded by limits derived from your host and shown by `make help`;
override them in `local.mk`, which is untracked and is where anything specific
to your machine belongs — so that host quirks never get baked into the repo:

```make
CPUS   = 8
MEMORY = 16g
JOBS   = 8

# Only if your Docker cannot create a private network namespace; see below.
# DOCKER_NETWORK = host
```

**The toolchain needs about 6 GB.** Building gcc links `lto1`, which is the
single largest allocation in the whole build; at 4.5 GB it is killed and the
error names neither memory nor the limit:

```
collect2: fatal error: ld terminated with signal 9 [Killed]
```

If you see that, raise `MEMORY` — and lower `JOBS`, which controls how many of
those links can peak at the same time.

Set `MEMORY` **below** your available RAM, not above it. A cap higher than what
the host actually has is not a cap: the host starts swapping long before the
container limit is ever reached, which is the failure it was meant to prevent.

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
- **Every container fails before running anything**, with

  ```
  runc create failed: unable to start container process:
  error during container init: open sysctl
  net.ipv4.ip_unprivileged_port_start ... permission denied
  ```

  You are running Docker inside an unprivileged container — LXC, including a
  Proxmox container, is the usual case. `runc` sets that sysctl while building
  a private network namespace, and `/proc/sys` is not writable there. Put
  `DOCKER_NETWORK = host` in `local.mk` so containers share the host's network
  instead. Nothing about the build depends on network isolation; the sources
  are hash-verified either way.

## What is not here yet

The only board is `virt-x86_64`, which is a real image with a virtual dataplane
(veth pairs behind a bridge) rather than a stub. Real switches land from M6; see
[MILESTONES.md](MILESTONES.md). The `full` and `slim` profiles are unfinished —
`minimal` is the one that boots today.
