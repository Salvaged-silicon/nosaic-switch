# NOSaic

A network operating system for **end-of-service-life switches and routers** — hardware
the vendor has abandoned, given a modern, open, maintained OS.

A mosaic is assembled from broken and discarded pieces into something better than the
originals came from. NOS is the first three letters.

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

`NATIVE=1` uses host tools instead of the container, which is faster for local
iteration but is not what CI does.

## Licence

Apache 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Images built by NOSaic may include third-party software under other licences, including
source-available vendor SDKs and firmware. Every recipe declares its licence and whether
it may be published, the build refuses to produce a publishable image containing anything
that may not be, and every image ships a NOTICE and an SBOM. A built image is therefore
mixed-licence, not pure OSI — NOSaic itself is Apache 2.0.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Adding a board is one self-contained directory
under `platform/`; there is no central file to edit.
