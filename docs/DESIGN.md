# NOSaic design

NOSaic is a network operating system for end-of-service-life switches and routers. This
document is the architecture; [MILESTONES.md](MILESTONES.md) is the order it gets built in.

## What the target hardware is like

End-of-service-life gear is a **long tail**, and that shapes almost every decision here:

- Many boards from many vendors, not two or three.
- Small flash and small RAM are normal, not exceptional.
- Silicon whose SDK is often hard to obtain, and hardware with no public documentation.
- No vendor support, so anything the OS cannot do for itself does not get done.

A design that suits three well-documented modern whiteboxes does not survive this. What
follows is aimed squarely at the long tail.

## Two governing rules

**1. The core has no board knowledge.** NOSaic builds, boots and is tested with no board
support in it at all. Board ports are additive and land one at a time; a virtual platform
is board number one and stays green in CI forever. If the core cannot build without a
board, the boundary is not real — and a core that quietly absorbs one board's constants
becomes impossible to port to the second.

**2. One architecture goes all the way to hardware first.** The VM and the first real board
are both x86_64, so cross-compilation is not on the critical path to a working switch.
Other architectures are bolted in afterwards, deliberately. A cross-architecture canary
builds in CI throughout so the architecture seam cannot silently rot into host assumptions.

## The axes

A board is described by three orthogonal axes plus a profile:

| Axis | What it selects |
|------|-----------------|
| **arch** | The CPU — toolchain, ABI, kernel architecture |
| **asic** | The switch silicon — which `nosd` provider is installed |
| **boot** | The bootloader — the installer envelope only; slots are the initramfs's job |
| **profile** | `full` / `slim` / `minimal` — package set and init system |

Board directories are the source of truth. The catalog is derived by scanning them, so
adding a switch means adding one self-contained folder and editing no shared file. That
matters for a project that expects board ports from people it has never met.

## Building from source

NOSaic builds its own cross-toolchains (crosstool-NG) and its own base system from
declarative per-package recipes, cross-compiled inside a pinned container. It does not
capture an existing distribution's root filesystem.

That is more work up front, and it is the difference between a project that can add its
tenth board and one that cannot. A captured base brings someone else's libc, init, kernel
version and assumptions; three captured bases bring three incompatible sets of them.

A recipe declares source and hash, patches, dependencies, build steps, installed files,
and services. It is the only place build knowledge lives — there are no per-board build
scripts.

### Packages

Each recipe produces a `.nos` package: an outer tar of `manifest.json` plus a compressed
payload, built reproducibly, hashed per file, tagged by architecture, and refused at
install time if it does not match the running system.

**One name, many providers.** The datapath daemon is always `nosd`. Implementations are
packaged per ASIC — `nosd-td2p`, `nosd-td2`, `nosd-prestera`, `nosd-virt` — each declaring
`provides: [nosd]` and `conflicts: [nosd]`. Units, CLI, config and documentation only ever
say `nosd`; the builder picks the provider from the board's ASIC, and the conflict makes it
an error to end up with two fighting over one chip. Providers are keyed to the *silicon*,
not the board, so one package serves every switch using that chip.

**A package owns the accounts it creates, and the files those accounts need.** A recipe
declares system accounts in `users:` with fixed numeric ids, and an `install:` entry may
name one in `owner:`:

```yaml
users:
  - {name: frr, uid: 101, gid: 101, home: /var/run/frr, shell: /sbin/nologin}
install:
  - {src: nosaic/frr.conf, dst: /etc/frr/frr.conf, mode: "0640", owner: "frr:frr"}
```

Without that a package could create an account and install a file that account cannot
read, which is not a hypothetical: FRR ships `frr.conf` mode 0640 and runs its daemons as
`frr`, and root-owned it meant `ospfd` started, read nothing, and reported *"OSPF is not
enabled"* — a permission error wearing a configuration error's clothes, on a daemon that
was running and supervised the whole time.

Ownership is resolved to numeric ids **at build time from the recipe's own `users:`**,
never from the build host's `/etc/passwd`. A package that took ownership from whichever
machine built it would install different files depending on where it was built, and the
account almost certainly does not exist on the builder anyway.

**Ownership in an image is proved, not overwritten.** The image build used to pass
`mksquashfs -all-root`, which guaranteed reproducibility against build-host ownership by
flattening everything to root — including ownership a recipe had deliberately declared,
and including `mksquashfs`'s own `-pf` override, so there was no way round it while it
stayed. It is gone. In its place the builder walks the composed tree and fails if any
file is owned by an id no recipe asked for by name. That is the stronger guarantee: a
build host uid leaking into an image is now an error that names the file, rather than
something silently painted over.

### Licensing

NOSaic is Apache 2.0. Built images may contain source-available vendor code and are
therefore mixed-license, not pure OSI.

This is enforced rather than documented. Every recipe declares `license:` and
`redistributable:`, both of which refuse omission rather than defaulting. The build will
happily produce an image containing non-redistributable components for local use, and
refuses to produce a *publishable* one. Every image ships a NOTICE and an SBOM.

## The same commands on every switch

Separate datapath implementations and a single set of commands are not in conflict, as
long as the split is in the right place: **separate implementations, one northbound
contract.**

```
  nosaic CLI  +  declarative config       board-independent; the only thing users touch
        │
        ├────────────►  switch-api        ports, VLANs, FDB, routes, nexthops, ACLs,
        │               (northbound)      counters, SFP — versioned, with an explicit
        │                    │            capability model
        │                    ▼
        │            per-ASIC nosd        vendor SDK, direct programming, or an
        │              (southbound)       in-kernel switchdev driver
        │
        └────────────►  platform-hal      thermals, fans, PSUs, LEDs, SFP EEPROM
                                          — box hardware, not forwarding
```

Four things keep this from decaying:

- **Two HALs, not one.** Forwarding and box hardware are different problems with different
  lifetimes. Conflating them produces a "hardware abstraction layer" that abstracts sensors
  well and forwarding not at all.
- **Capabilities are explicit and the CLI fails loudly.** No two chips support the same
  feature set — ECMP width, ACL slices, IPv6 route capacity, port breakout all vary. A
  board advertises what it can do, and an unsupported operation is reported. Silently doing
  less is the failure mode that costs someone a night of debugging.
- **Ports are named, not numbered.** Config says `swp1`. The board's port map is the only
  place that becomes a physical or ASIC port number.
- **The contract is defined against the virtual platform, before any real silicon exists.**
  Otherwise the first chip ported defines the contract by accident, and every chip after it
  contorts to fit a model chosen for unrelated reasons.

**The CLI is not always the same program, and that is the part that nearly broke this.**
The Go toolchain has ppc64 and ppc64le and has never had 32-bit big-endian PowerPC, so a
board on that architecture cannot run the Go CLI and runs a C one instead. For a while
that CLI implemented `config`, `verify` and `platform` and nothing else, so the switch
genuinely did not answer the same commands as its neighbour in the same rack — the claim
above was false on the one board that tested it.

Two implementations are acceptable; two vocabularies are not. Both now ask the same ops on
the same socket and print the same columns, and the two are checked against each other by
running them side by side on a board that can host either: `show caps` and `show ports`
come back byte-for-byte identical. An architecture the compiler cannot reach is not a
reason for a switch to be operated differently.

## Image layout and upgrades

The build emits an immutable image. The switch mounts it read-only under an overlay and
keeps two of them, so an upgrade is always reversible.

```
  p1  bootloader / EFI       grubenv, Aboot boot-config, or U-Boot environment
  p2  slot A                 image.sqsh — read-only, immutable, hashed
  p3  slot B                 image.sqsh
  p4  data                   persistent, shared across both slots

  (a board whose bootloader owns the whole disk keeps the same four things as
   files on the bootloader's filesystem, loop-mounted, with identical semantics)

  /             overlayfs
     lower  =  active slot's squashfs         read-only
     upper  =  /mnt/data/slot-<a|b>/upper     per-slot writable layer

  /mnt/data/config/                           SHARED across slots
```

**What is shared and what is not is the load-bearing decision.** Config is shared: if it
lived in the slot, upgrading would lose it, which defeats the purpose. Package overlays are
per-slot: a component hot-patched onto the running slot must not leak into the other
slot's known-good state, or rollback stops meaning anything.

Config is a **document, not a diff** — a declarative file rendered to running state at
boot, not hand-edited system files captured in an overlay. That is what makes rolling back
to an older image while keeping current config safe.

**Upgrade:** write to the inactive slot, verify its hash, mark it a *trial*, reboot. The
system confirms itself healthy and the trial commits; if it never confirms, the bootloader
falls back. "Healthy" means the switch is doing its job — ports linked, datapath
programmed, adjacencies established — not merely that init came up. A switch that boots
but does not forward must fail its trial.

**Slot control does not live behind the boot backend, and the expectation that it would
was wrong.** The plan assumed each bootloader would need its own implementation — GRUB
counters, U-Boot `bootcount`, Aboot `boot-config`. In practice the initramfs reads the
pointer and chooses, so the bootloader never learns that slots exist and the same trial,
counter and rollback code runs on every board. Aboot's backend contains no slot logic at
all.

**Where the slots live does vary, and that is confined to two lookups.** Most boards give
them partitions. A board whose bootloader owns the whole disk keeps them as files on the
bootloader's own filesystem, loop-mounted — the Arista 7050SX2's eMMC is fully allocated
to Aboot's FAT, and Aboot resolves `flash:` by matching on the controller, so adding
partitions could leave it booting from the wrong one. Everything above the lookup is
identical.

**The kernel and initramfs are outside A/B.** A slot holds a root filesystem; the kernel
and initramfs live where the bootloader can reach them without understanding our layout,
there is one of them rather than two, and a change to either is written in place and
cannot be rolled back. An upgrade spanning both is only half atomic, and anything a trial
depends on must live in the slot.

On the smallest boards two slots may not fit. There, `nosaic upgrade` degrades to a
non-atomic path and says so, rather than pretending.

## Documentation, per switch

A reader arrives with one of three questions, and should not have to work out which page
answers it. So every board carries the same three, inside its own directory:

| Page | Audience | Contains |
|---|---|---|
| `docs/install.md` | Somebody holding the switch | Console, getting the image on, first boot, going back to the vendor OS, and the failures seen on *this* board |
| `docs/build.md` | Somebody making an image | Only what is board-specific — firmware, modules, device tree, profile. The generic build is in `BUILDING.md` and repeating it would let it drift |
| `docs/hardware.md` | Somebody changing the datapath | Block diagram, boot chain, port map, the register regions our code touches, the platform HAL, and the quirks |

They live with the board rather than in a central tree, because a board port is one
self-contained directory: adding one is adding a directory, never editing a shared file
that every other contributor is also editing. `docs/switches.md` is **generated** from the
board directories by `make docs`, and `nosaic check` fails if it has gone stale — so the
index cannot drift from the boards it indexes.

The template pages ship marked as unfilled, and the check fails while that marking is
still present. A board therefore cannot merge carrying a page nobody wrote, which is the
common way per-board documentation becomes a directory of headings.

## Reverse engineering

Much of this hardware is undocumented, and working it out is a large part of the project.
It lives in its own repository per switch, linked from the board's directory. The OS tree
stays about the OS.

**Where the line falls.** `docs/hardware.md` documents the board *as NOSaic drives it*:
topology, boot chain, port map, the register regions our code touches, and the quirks that
cost somebody a day. The *investigation* — traces, hypotheses, eliminated leads, and
anything derived from vendor binaries or headers — stays in the RE repository. That is not
only a licensing rule, though it is that too: a register table transcribed from a vendor
header is that vendor's, and a published image containing non-redistributable material is
the failure mode the `redistributable:` gate exists to prevent. It is also an editorial
one. The investigation is a record of how something was learned, and it is far longer than
what was learned; mixing the two makes the hardware page unreadable for the person trying
to fix a port at 2am.

Vendor SDK source is never copied into this tree. Reference it by `file:line`.

## Prior art, and what it taught

NOSaic is a clean rebuild, not a refactor. It follows an earlier project, EdgeNOS, which
runs production switches today and is the reason several decisions here are non-negotiable.
EdgeNOS is not being converted or deleted; it keeps its boards until NOSaic has replaced it
on real hardware and been proven there.

Four lessons carried directly into this design:

**A captured base is a borrowed problem.** EdgeNOS never built a root filesystem. Each
board's base was taken from a different upstream, so three boards meant three libcs, three
init systems, three sets of assumptions and no common ground to stand on. Building from
source is the expensive answer, and it is the one that scales past the third board.

**"ASIC-agnostic" has to be enforced, not asserted.** EdgeNOS had a `core/datapath`
directory that included a Broadcom chip header and hard-coded a 52-port constant. Nobody
intended that; it accrued because there was no board-free build to break. Hence rule one:
the core is built and tested with no board in it, and a virtual platform is board number
one.

**Silent degradation is worse than an error.** EdgeNOS's second board inherited a copy of
the first board's L3 configuration script with ECMP support quietly removed. Nothing
recorded the difference. Hence an explicit capability model, and a CLI that reports what a
board cannot do rather than doing less without saying so.

**A rule enforced by memory is already broken.** Three separate vendor SDK patch sets
existed in that tree and no build script applied any of them. Hence the licensing gate, the
dependency resolution and the structural invariants all live in `make check`, running in CI
from the first commit rather than being added once the tree is full.
