# NOSaic

[![ci](https://github.com/Salvaged-silicon/nosaic-switch/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Salvaged-silicon/nosaic-switch/actions/workflows/ci.yml)

A network operating system for **end-of-service-life switches and routers** — hardware
the vendor has abandoned, given a modern, open, maintained OS.

> **Status: early. No board is supported yet.**
> This README advertises only what is merged, working and finished. "Supported" means a
> board somebody else can install and run — see [Where it has got to](#where-it-has-got-to)
> for what actually runs today, which is more than that table shows.
> [docs/DESIGN.md](docs/DESIGN.md) is where it is going and
> [docs/MILESTONES.md](docs/MILESTONES.md) is what lands when.

## Supported hardware

None yet.

A board reaches this table when it installs itself, survives a power cycle, and passes
CI — not when it works on the bench. Three boards exist and none has cleared that bar:
two in bring-up and one only declared. All are in
[docs/switches.md](docs/switches.md).

| Board | Arch | ASIC | Boot | Status |
|-------|------|------|------|--------|
| _none_ | | | | |

Every board carries at least three pages — **install**, **build** and a deep
**hardware** reference with its architecture, port map and registers — plus a **todo**
listing what is left, separated into what stops it working and what does not. They live
in the board's own directory, and [docs/switches.md](docs/switches.md) indexes them from
what each directory actually contains.

## Where it has got to

The first real board is the **[Arista DCS-7050SX2-72Q-R](platform/arista-7050sx2-72q/)** —
Broadcom Trident2+ (BCM56860), x86_64, Aboot. On that switch, measured rather than
assumed:

- it boots NOSaic built entirely from source, over the network into RAM;
- the switch chip comes out of reset and initialises, driven from userspace with **no
  Broadcom kernel modules**;
- front-panel ports appear on the Linux stack, and **FRR holds OSPFv2 and OSPFv3
  adjacencies** with two different vendors' routers;
- **40G works as well as 10G**: both QSFP+ cages link at 40000 and pass traffic to two
  different neighbours, from a cold boot, with no manual step;
- the kernel routing table is mirrored into the ASIC — 96 route entries, by the chip's
  own accounting;
- **forwarding happens in silicon**: 100 packets routed through the box raised the chip's
  port counter by 101 and the CPU's by 44, which is the background protocol traffic and
  nothing like 100;
- **its addressing and OSPF configuration come back on their own** after a power cycle —
  loopback, every routed port, and the routing daemons, with nothing typed in;
- fans, temperatures, PSUs and transceivers read and control through the platform HAL,
  with a closed-loop fan curve.

Two things stand between that and a supported board, and only one of them is about
the switch being a switch.

**The control plane is honest now, and was not.** Every frame destined for this
switch is punted through the CPU by the tap bridge. That path used to carry
**about twenty packets a second**; it now carries **500/s at zero loss**, which
is where the test stopped rather than where the path does. The cause was not
the silicon and not the SDK — the daemon was rescanning the entire kernel
routing table, and issuing a netlink round trip, *once per received packet*.
Two things were fixed to get there and only one of them mattered for
throughput: the datapath now takes interrupts from the chip instead of polling
for them, which recovered a whole CPU core and moved the packet rate not at
all, and then the per-packet rescan became per-second.

**The switch was configured with `ip` and `vtysh`, not with NOSaic's own CLI.** The
`nosaic show ports`, `interface` and `route` commands exist and run against the
virtual datapath in CI; they have not been run against silicon. Until the same
commands work unmodified on both, the claim below about one CLI everywhere is a
design commitment rather than a demonstrated fact — which is why it is the gate on
this board rather than something to assume.

It is reachable over the network now: dropbear is packaged, host keys are
generated on the box rather than shipped, and authorised keys come from the
board's gitignored `config/authorized_keys`. Login takes 0.096 s where the
9600-baud console took minutes.

Everything else outstanding is in each board's own list:
**[7050SX2](platform/arista-7050sx2-72q/docs/todo.md)** ·
**[virt-x86_64](platform/virt-x86_64/docs/todo.md)** ·
**[AS5610-52X](platform/edgecore-as5610-52x/docs/todo.md)**.

## The next board

The **[Edgecore AS5610-52X](platform/edgecore-as5610-52x/)** is declared and
nothing runs on it. It earns a mention here because of what it tests rather than
what it does: it is the first board that is not x86_64, and it changes three axes
at once — **PowerPC e500v2**, **Trident+ (BCM56846)**, **ONIE on NOR flash**.
Every claim above about architecture-neutrality has so far been a design
commitment rather than a demonstrated fact, because both existing boards are
x86_64.

Two of the three axes already exist in the tree: `arch/powerpc` is fully
specified — soft-float, big-endian, with an instruction audit that fails any
build containing an opcode an e500v2 cannot execute — and the `onie` and `uboot`
boot backends are written. The ASIC does not: there is no `nosd-tdp`, though
OpenBCM 6.5.24 already carries the chip, so it is a new package rather than a
new SDK.

Starting a board is one command and no change to anything central:

```sh
nosaic board scaffold edgecore-as5610-52x \
  --vendor edgecore --model as5610-52x \
  --arch powerpc --asic tdp --boot onie-sfx --console ttyS0,115200
```

`nosaic check` then refuses the board until its documentation is real rather
than the template's, which is how the three pages every board carries stay
worth reading.

If you have one of these switches, the ordered path from a rack to a forwarding
box is **[the walkthrough](platform/arista-7050sx2-72q/docs/walkthrough.md)**.
How it all works, with diagrams, is in
**[architecture](platform/arista-7050sx2-72q/docs/architecture.md)**; the development
loop that produced it is in
**[running](platform/arista-7050sx2-72q/docs/running.md)**.

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

These are the commitments the design is built on. Where one is already load-bearing the
section above says so; where it is not yet demonstrated on silicon, it says that too.

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
      BOARD               INSTALLS BY                                        STATUS
  1.  arista-7050sx2-72q  a SWI booted by Aboot: copy to flash and point...  bringup
  2.  virt-x86_64         no installer: QEMU is given the kernel...          bringup

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
