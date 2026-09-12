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

**Per-board vendor data is generated, not shipped.** Four generators in
`tools/`, each read off a switch running the vendor OS, each landing in
`config/`, all four gitignored. Run them once per switch:

| generator | produces | without it |
|---|---|---|
| `mkportmap.sh` | `portmap.conf` — which lane reaches which cage, and each copper PHY's MDIO address | the datapath refuses to start: "no port map, so the chip would initialise and reach no front-panel cage" |
| `mkpolarity.sh` | `polarity.conf` — SerDes lane polarity, the lane swizzle and the firmware mode | cages link at 40000 and carry **zero frames**, with no error on either end |
| `mkretimer.sh` | `retimer.conf` — the DS100KR800's amplitude and de-emphasis | Et51 and Et52 receive perfectly and transmit nothing; the far end never links |
| `mkserdes.sh` | `serdes.conf` — this board's transmit tap profile | the cage behind the repeater links and the far end reports a remote fault |

```sh
cd platform/arista-7050tx-64/tools
./mkportmap.sh  <switch-ip> > ../config/portmap.conf
./mkpolarity.sh <switch-ip> > ../config/polarity.conf
./mkretimer.sh  <switch-ip> > ../config/retimer.conf
./mkserdes.sh   <switch-ip> > ../config/serdes.conf
```

⚠ **A guessed map satisfies every bandwidth rule the chip enforces and reaches
none of the right cages**, so the failure is a dead port rather than an error.
Every one of the four fails silently in its own way — the table above is what
each silence looks like.

⚠ **A generator verified against the reference can still be missing a whole
family.** Both the port map and polarity generators reproduced this board's
working configuration "byte for byte" while emitting no lane maps at all: a
diff only compares the keys a generator emits, and a family absent from both
sides cannot show up as a difference. Compare the *whole* reference file
against the *whole* generated set, and watch the counts each generator prints
on stderr.

**Shipped board configuration**, by contrast, is committed and needs nothing
from you: `asic.conf` (SDK properties and the tap declarations), `network.conf`,
`frr.conf`, and `statusleds.conf` — the last of these unusually, because
nothing in this board's lamp map is the vendor's. See its header for why.

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
