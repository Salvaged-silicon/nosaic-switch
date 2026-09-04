# Building an image for the 7050SX2-72Q

The general build is in [docs/BUILDING.md](../../../docs/BUILDING.md). Only what
is specific to this board is here.

## The short version

```sh
make toolchain ARCH=x86_64
make image BOARD=arista-7050sx2-72q PROFILE=minimal ARGS=--ram-boot
```

That produces, in `out/images/arista-7050sx2-72q/`:

```
  NOSaic-<version>-arista-7050sx2-72q.swi    what Aboot boots
  boot-config                                the line that points at it
  vmlinuz  initramfs.cpio.gz  rootfs.sqsh    the pieces inside it
  disk.img                                   for the flash install route
```

Drop `ARGS=--ram-boot` for an image intended for flash — which is how this board
normally runs, and what [install.md](install.md) describes. Without it the root
filesystem is a separate `rootfs.sqsh` installed into a slot and the SWI carries
only the kernel and initramfs, which is why the SWI is ~15 MB rather than ~81 MB
and why the board gets that RAM back. Use `PROFILE=minimal`
for now: `board.yml` says `full`, but only `minimal` has ever been booted on
this board.

This board is x86_64, the same architecture the virtual platform proves on every
commit, so the toolchain, glibc, kernel and userspace are already tested —
nothing here is a fresh cross-compilation problem.

## Building one package at a time

The datapath is the thing you will iterate on, and it does not need a whole
image:

```sh
make pkg PKG=nosd-td2p ARCH=x86_64      # the datapath, ~2 min
make pkg PKG=frr       ARCH=x86_64      # the routing stack
make check                              # tests, vet, gofmt
```

See [running.md](running.md#8-updating-just-the-datapath) for pushing a rebuilt
`nosd` onto a running switch without a reboot.

## What this board pulls in that a server image does not

| Package | What it is | Licence |
|---|---|---|
| `openbcm` | Chip support for BCM56860, SDK **6.5.24** | Broadcom OpenBCM — redistributable, see below |
| `nosd-td2p` | The Trident2+ datapath, including a userspace BDE | Apache 2.0 |
| `frr` | zebra, ospfd, ospf6d, bgpd | GPL-2.0-or-later |

plus the chain FRR needs: `json-c`, `libyang`, `pcre2`, `protobuf-c`,
`readline`, `ncurses`.

`openbcm` is a **`build_depends:`**, not a `depends:`. It is linked into
`nosd-td2p` and has no business being installed as well — carried as a runtime
dependency it made a 794 MB image against a 768 MB slot.

## The SDK, and why an image carrying it may be published

Broadcom's OpenBCM licence (`Legal/LICENSE` in that tree) grants a worldwide,
royalty-free, perpetual licence to reproduce and distribute the software, and
for source to create and distribute derivative works. That is what makes a
NOSaic image containing it shippable rather than local-only, and it is why the
datapath goes through the SDK rather than around it.

One obligation comes with it and it is not optional: **every distributed copy
must reproduce all proprietary notices**. Removing or obscuring them is
expressly forbidden, so the SDK's own licence and notices ship in the image's
NOTICE, and the recipe declares `redistributable: true` on that basis rather
than on assumption.

The source is mirrored at
[Salvaged-silicon/OpenBCM](https://github.com/Salvaged-silicon/OpenBCM) so the
build does not depend on an upstream that could move. **That repository carries
ten SDK versions side by side**, 6.5.16 to 6.5.27, so every path names the
version it means. The recipe pins **6.5.24** — not because it is the newest,
which it is not, but because it is the version this board's datapath was proven
with. `sdk-6.5.24/src/soc/mcm/bcm56860_a0.c` is this chip's support.

A newer version is not automatically a better one here: the SDK is the piece
whose unmodified chip initialisation is the entire reason for taking this route,
so the version that has been run against real silicon wins over the one with the
higher number. SDK source is referenced by `file:line` and never copied into
this tree.

## Kernel

The fleet kernel plus a board fragment. What this board needs:

- `CONFIG_TUN` — the tap bridge is how ports reach the Linux stack. Without it
  `nosd` cannot open `/dev/net/tun` and no port is on the network stack at all.
- `CONFIG_I2C_PIIX4` — without it the fan CPLD cannot be reached.
- `tg3` for the management port (`14e4:1682`).
- `memmap=64M$0xd0000000` on the command line, from `board.yml`, reserving the
  DMA pool the userspace BDE claims through `/dev/mem`. Without it the datapath
  has no memory it can hand to the chip's DMA engine.

The Arista SCD (`3475:0001`) needs no kernel driver: NOSaic drives it from
userspace through its PCI BAR.

## Site configuration is not in the repository, but it is in the build

`portmap.conf` and `polarity.conf` are absent from this repository — the numbers
are the vendor's, read off a machine that already has them, so they are not ours
to publish. That is a statement about the repository, not about the image:
generate them once, drop them in `platform/arista-7050sx2-72q/config/`, and the
image builder copies them into `/etc/nosaic` like any other board
configuration. `.gitignore` keeps them untracked.

They are board data rather than per-unit data — every switch of this model has
the same lane map and the same PCB polarity — so one generation serves all of
them. An image built without them boots and has no datapath. See
[running.md](running.md#4-site-configuration).

`config/asic.conf` **is** in the repository, because everything in it is
board-independent chip configuration rather than vendor data: interrupt mode,
port defaults, the SerDes lane map and its per-macro exceptions, and the `tap_`
declarations.

## Reproducibility

Packages are byte-identical across builds — sorted archive members, zeroed
uid/gid, `SOURCE_DATE_EPOCH` — and CI checks it by building twice and comparing
hashes. If a package hash moves without a recipe change, something is leaking
the clock or the build path.

## Verifying before you install

Aboot boots an image over HTTP without writing flash, and during bring-up that
is the right way to try one. See [running.md](running.md). Nothing in this
section is a substitute for having the EOS SWI saved.
