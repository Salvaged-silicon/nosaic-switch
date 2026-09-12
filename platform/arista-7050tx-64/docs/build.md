# Building an image for the Arista 7050TX-64

Only what is specific to this board. The general build — toolchain, packages,
image, VM — is in [docs/BUILDING.md](../../../docs/BUILDING.md), and repeating
it here means it will drift.

These commands have been run and the image they produce boots on the switch.

## The short version

```sh
make toolchain ARCH=x86_64
make packages  ARCH=x86_64 PROFILE=minimal
make image     BOARD=arista-7050tx-64
```

Lands in `out/images/arista-7050tx-64/`.

The toolchain and the base packages are shared with `arista-7050sx2-72q` — same
architecture, same profile — so on a machine that has already built for that
board only the image step is new.

## What this board needs that the generic build does not

**`nosd-td2` — the Trident2 datapath.** This board declares `asic: td2`, and the
image builder resolves that to whichever package provides `nosd` for it. It is
derived from `datapath/td2p/` — the same CMICm generation, the same
architecture, the same userspace BDE — reusing `datapath/common/` rather than
forking it, and adding `phy.c` for the 48 copper PHYs.

⚠ **`make packages` will not rebuild it after a source edit.** The package is
already built, so the recipe is skipped and the image silently carries the old
binary — which looks exactly like a fix that did not work. Force it:

```sh
rm -rf .cache/pkg/nosd-td2
make pkg PKG=nosd-td2 ARCH=x86_64
```

The cache holds object files as well as the package, and a stale `props.o`
there is what turns a new function in `datapath/common/` into an undefined
reference at link time.

**The Broadcom SDK is a build dependency and is never shipped.** `openbcm` is
staged for the compiler and stays out of the image. Two consequences worth
knowing before debugging a strange failure:

- The SDK's *own* compile-line defines are captured into `sdk-defines.txt` and the datapath must build with that exact set. A wrong set compiles, links, runs, and corrupts every struct the SDK shares with us.
- The SDK is fetched and hash-pinned, never committed.

**Per-unit board data is generated, not shipped.** The port map and SerDes
polarity are read off a switch running the vendor OS by the scripts in `tools/`
and land in `config/`, which is gitignored. A build without them produces an
image whose datapath reports itself unconfigured — which is the right failure.
A guessed map satisfies every bandwidth rule the chip enforces and reaches none
of the right cages.

## Profile

`minimal` (s6). Matches the sibling `arista-7050sx2-72q`, and this board has no
more flash to spare than that one does.

⚠ `profile` is a claim about what the board can run, not a preference. On the
SX2, declaring `full` while every working image was built `minimal` produced a
switch that reached "Started ospf6d" and never came up. Do not change it here
without building and booting what you changed it to.

## Verifying before you install

This board's recovery path is a console cable and the vendor OS, so the checks
are worth the minutes.

- `make check` — the repository invariants, including that this board's documentation is not a template and the switches table is current.
- Confirm the SDK is **absent** from the image, not merely unused.
- Confirm the datapath package is present at all: an image missing `nosd` is the one that boots, answers ssh and silently does not forward — the exact shape of image used to prove rollback works, so it is easy to build by accident.
- `boot --testonly flash:/<image>.swi` at the Aboot prompt stages the kernel and initramfs through kexec and stops without jumping. It catches a malformed or truncated image before it costs a boot.
