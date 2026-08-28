# NOSaic

A network operating system for **end-of-service-life switches and routers** — hardware
the vendor has abandoned, given a modern, open, maintained OS.

> **Status: early. Nothing is supported yet.**
> This README advertises only what is merged and working. Right now that is the build
> skeleton and its checks. See [docs/DESIGN.md](docs/DESIGN.md) for where it is going
> and [docs/MILESTONES.md](docs/MILESTONES.md) for what lands when.

## Supported hardware

None yet. The first board will be `virt-x86_64` — a virtual platform that boots under
QEMU — followed by real silicon.

Boards appear in this table when they boot, forward traffic, and pass CI. Not before.

| Board | Arch | ASIC | Boot | Status |
|-------|------|------|------|--------|
| _none_ | | | | |

Every board that lands carries three pages — **install**, **build** and a deep
**hardware** reference with its architecture, port map and registers. They live in the
board's own directory, and [docs/switches.md](docs/switches.md) indexes them.

## Bootloaders

Switches disagree about how an image gets onto them, so the bootloader is one of the four
things a board declares — beside its architecture, its ASIC and its profile. You do not
pick it when you build; the board already knows, and `boot:` in its `board.yml` says so.

| `boot:` | Used by | What NOSaic emits |
|---------|---------|-------------------|
| `onie-sfx` | Most whitebox switches (Edgecore, Delta, Celestica) | A self-extracting installer ONIE runs |
| `aboot` | Arista | A `.swi` Aboot loads, plus the `boot-config` pointing at it |
| `uboot` | Older boards with no ONIE | A FIT image, loadable over TFTP or from flash |
| `virt` | QEMU | Nothing — the kernel and disk are handed to QEMU directly |

Adding another is a backend in `internal/boot/` and a line in the board — not a change to
the image builder, which never learns what a bootloader is.

U-Boot boards must also state `u_boot_load` and `u_boot_entry`. There is no default,
because the right address depends on where that board's RAM is and a wrong one produces a
board that hangs with nothing on the console. A board that omits them is rejected before
the build starts rather than after it.

## What makes it different

- **Built from source.** NOSaic builds its own toolchain and its own base system. It does
  not capture someone else's root filesystem and inherit their assumptions.
- **The core has no board knowledge.** NOSaic builds, boots and is tested with no board
  support in it at all. If the core cannot build without a board, the boundary is not real.
- **The same commands on every switch.** One CLI, one declarative config, one northbound
  contract. Chips differ underneath; what you type does not.
- **Honest about capability.** Silicon varies. Every board advertises what it supports, and
  an unsupported operation is reported rather than silently doing less.
- **A/B images with rollback.** An immutable image under an overlay, two slots, trial boots
  and automatic rollback. Config is shared across slots; package overlays are not.

## Building

You need **Docker** and **make**. Nothing else — the toolchain lives in a pinned container.

```sh
git clone https://github.com/salvaged-silicon/nosaic-switch
cd nosaic-switch
make check          # validate the repo
make nosaic         # build the CLI into out/
```

To build an image, name the board — `nosaic build <board>`. Run it without one and it
lists what there is to choose from, and offers to pick if you are at a terminal:

```
$ nosaic build
Which switch?
      BOARD        INSTALLS BY                                       STATUS
  1.  virt-x86_64  no installer: QEMU is given the kernel...         bringup

number or name:
```

In a script or in CI it prints the same list and exits, rather than waiting for an answer
nobody is there to give.

Those two are quick. Building the OS itself means building a cross-toolchain and
a libc from source, which is measured in hours rather than minutes — the whole
sequence, what it costs, and how to boot the result in a VM you can log into are
in **[docs/BUILDING.md](docs/BUILDING.md)**.

```sh
make vm BOARD=virt-x86_64     # once built: a console on the running system
```

`NATIVE=1` uses host tools instead of the container, which is faster for local
iteration but is not what CI does.

## Source archives

Every upstream source is pinned by SHA-256 and mirrored at
[salvaged-silicon/nosaic-sources](https://github.com/Salvaged-silicon/nosaic-sources).

NOSaic runs on hardware whose vendors walked away, and source archives are abandoned on
the same timeline — several components here (gcc, glibc, binutils, gmp, mpfr, isl) have no
repository on GitHub at all and are served from FTP mirrors that come and go. Because
everything is hash-verified, a mirror cannot serve different content than upstream would,
so falling back to one costs nothing in trust.

Upstream is still tried first: it is the real provenance, and a build that quietly stopped
touching it would never notice it rotting.

## Vendor SDKs

Silicon that needs a vendor SDK gets one, where the licence permits shipping it.
Both are forked into this organisation so a build never depends on an upstream
that could move:

| Fork | Upstream | Used for |
|---|---|---|
| [OpenBCM](https://github.com/Salvaged-silicon/OpenBCM) | Broadcom-Network-Switching-Software/OpenBCM | Trident2+ (BCM56860) and the rest of the XGS line |
| [OpenMDK](https://github.com/Salvaged-silicon/OpenMDK) | Broadcom/OpenMDK | CDK-driven older parts |

Broadcom's licence grants a worldwide, royalty-free, perpetual right to
reproduce and distribute the software, and for source to create and distribute
derivative works — which is what makes an image containing it publishable. It
also requires that **every distributed copy reproduce all proprietary notices**,
so those ship in the image's NOTICE. No SDK source is copied into this
repository; it is referenced by `file:line`.

## License

Apache 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Images built by NOSaic may include third-party software under other licenses, including
source-available vendor SDKs and firmware. Every recipe declares its license and whether
it may be published, the build refuses to produce a publishable image containing anything
that may not be, and every image ships a NOTICE and an SBOM. A built image is therefore
mixed-license, not pure OSI — NOSaic itself is Apache 2.0.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Adding a board is one self-contained directory
under `platform/`; there is no central file to edit.
