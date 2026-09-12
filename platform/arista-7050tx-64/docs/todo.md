# Arista 7050TX-64 — what is left

Ordered by whether the switch works without it, not by effort. The board status
as a whole is in [the README](../README.md).

This port is at `bringup`: NOSaic boots on the switch, initialises the Trident2
and brings its three cabled 40G links up, and forwards nothing. The split below
is by what blocks what.

## Done

- **An image builds.** `make image BOARD=arista-7050tx-64` produces a 14.6 MiB
  SWI with a 63.1 MiB root filesystem, from the shared x86_64 toolchain and the
  `minimal` profile. Not booted.
- **`datapath/td2` and `recipes/nosd-td2` exist and build.** Derived from
  `td2p` — PCI device `0xb855` and revision `0x03`, both read off the board.
  The image picks it up and the "no datapath" warning is gone. Never run on
  hardware.

- **`config/asic.conf` and the generators are written**, and both generators
  were checked against this board rather than assumed: fed the capture from a
  switch running the vendor OS, `mkportmap.sh` and `mkpolarity.sh` reproduce
  the working configuration **exactly**, byte for byte. Two transforms had to
  be right for that, and neither is obvious:

  - the capture is a **61-port** board and this one is 52, so the QSFP cages
    collapse from four 10G lanes to one 40G port;
  - a 40G port's polarity is a **bitmask over its four lanes**, not the first
    lane's value — cage 3 is `0xd`, not the `0x1` its first lane carries — and
    a cage that appears once in the capture is already collapsed and must be
    passed through untouched.

  Getting either wrong produces a port that trains and then errors, which
  reads as a marginal cable.

- **The CMIC identity is no longer hardcoded to one chip.** Two places expected
  `0x0002b860` and would have reported this board's `0x0003b855` as a chip that
  did not match. Derived from PCI configuration space now, which keeps the
  cross-check the constant was there for — two independent paths agreeing —
  without baking in silicon. A core change, in its own commit.

- **The copper PHY layer is written** — `datapath/td2/phy.c`. The SDK owns the
  PHY itself and downloads its firmware given `load_firmware` and
  `phy_bus_i2c_<n>`; what it does not do is keep the chip's MAC side agreeing
  with what the PHY negotiated, which is the failure that reads as a dead cable.
  Ports are discovered from the properties rather than a number range, matched
  once and re-checked when a link drops, and the MDIO budget is bounded to four
  reads a second. **Never executed** — see below.

## Fixed on the hardware, 2026-09-11/12

Five faults, found by booting it. Each one had passed every check the build and
the box could make.

- **The fan controller was addressed on the wrong SMBus bus.** `0x60` is on bus
  1 here, the CPU card's, and the driver carried the sibling's bus 0 as a
  constant. Four refusals a cooling cycle from a healthy controller, on a box
  whose thermal failure mode is silent — and since the thermal loop returns an
  error rather than continuing, the service restarted on a loop and flooded the
  console. Placement is board data now. Four sensors read, and they agree with
  what EOS reports.
- **The initramfs probed for flash once, half a second too early.** This board
  boots off USB and the stack enumerated it at 25.7 s against a probe at 25.2 s.
  The boot stopped at `unknown slot 'a'`, which reads as a corrupt install.
- **A port map read off this switch is unit-suffixed and nothing was resolving
  the suffix.** 278 properties loaded, counted, printed — and invisible. See
  [hardware.md](hardware.md#port-map).
- **The cage control table was the sibling's**: 54 entries at `0xa010` instead
  of 4 at `0xa100`, writing across this board's per-cage LED block and leaving
  the real cages in reset and low power.
- **`upgrade install` wrote slots where the initramfs does not look.** It
  reported success, marked a trial, and the next boot rolled back for want of a
  file that was on the box — with every visible signal saying the upgrade had
  worked.
- **The generators never emitted the SerDes lane maps**, so three 40G links came
  up at 40000 and received zero frames. The PCB does not route the four lanes to
  a QSFP connector in order. Adding them started receive on both SX2 links.
  ⚠ Both generators had been verified against the working configuration and
  reproduced it "byte for byte" — a diff cannot show a family that is absent
  from both sides.
- **The board shipped no `frr.conf`**, so three routed links carried no routing.

## Blocking — NOSaic does not forward on this board without these

- **et52 to the Edgecore AS5610 is down in one direction, and the evidence says
  it is physical.** Both ends were read directly for this — the AS5610 takes
  `ssh -i ~/.ssh/id_ed25519 root@10.10.35.2`, which its board ships the key for.

  | | this board (et52, port 61) | AS5610 (swp52, port 52) |
  |---|---|---|
  | link | **up** — PCS locked on their light | **down** |
  | frames | `in-nuc=0`, `out-nuc` climbing | `in-nuc` frozen at 8161 |

  Asymmetric link state is the signature: we lock onto their transmitter, they
  never lock onto ours. Their `in-nuc=8161` says this link carried traffic
  historically, so the cabling is right and the cage numbering is right.

  What has been ruled out, each with the far end as the witness rather than a
  local status:

  - **Configuration on this end.** Port 61 now matches the predecessor's
    working file on every property — lane maps, polarity, firmware mode,
    autoneg — and the identical code brings up et49 and et50.
  - **Clause-73 autoneg.** Tried set and clear; `swp52` stayed down for both.
  - **A stuck link.** Full chip re-initialisation on *each* end independently,
    which retrains the port. No change either time.
  - **The optic being held down.** Cage 4's control register takes writes
    correctly (`0x108 -> 0x140` with TX_DISABLE set, `-> 0x100` clearing it)
    and a power cycle of the module changed nothing.
  - **The far end being broken generally.** Its `swp49` and `swp51` are up and
    receiving, so its receive path works on its other two 40G cages.

  That leaves one direction of the path between the two cages: the AOC, or one
  end's optic. **The next step is physical and takes a minute:** move this
  board's end of the cable from Et52 to Et51, the empty cage. If the link comes
  up there, this board's cage 4 or its end of the optic is at fault; if it stays
  down, the cable or the far end's cage is. Et51 is port 57 and already has its
  lane map and polarity generated — it needs a `tap_et51`, an address and an
  OSPF cost, all one line each.

- **The cage-word decode table is the sibling's and does not fit this board.**
  `internal/platformhal/scd/transceiver.go` knows `0x47`, `0x1c0` and `0x180`;
  this board reads `0x108` for a cabled cage, `0x105` for an empty one and
  `0x100` for a cabled one whose latched status has been cleared. So every cage
  here decodes as "undetermined", which is honest but useless — and it is the
  fourth thing in this driver that was one board's constant. Measuring the four
  values on this board would make presence reporting work, and it is read-only.

- **No OSPFv3 adjacency.** Our side is configured and running: `ospf6d` answers,
  all three taps have link-local addresses, and each interface declares
  `ipv6 ospf6 area 0.0.0.0`. The 7050SX2 forms OSPFv2 with us over both links
  and nothing over v3, and it ships no `config/frr.conf` of its own — its
  routing configuration comes from `/mnt/data/config/frr.conf` on that box.
  Likeliest that it simply does not run OSPFv3 on these links.

- **No flash backup since the board's contents changed.** The existing backup
  predates everything written to `/mnt/flash` since, and the vendor images on
  that flash are the recovery path. Take one.

## Not blocking — the switch runs, short of these

- **The chassis status lamps have no map.** The thermal loop reports, once per
  cycle, that `/etc/nosaic/statusleds.conf` is not configured — and points at
  `platform/arista-7050sx2-72q/tools/mkstatusleds.sh`, which is the sibling's
  tool and the sibling's register bits. This board needs its own, or the
  message needs to stop naming one that does not apply here.

- **`boot-config` still points at EOS, deliberately.** NOSaic is booted as a
  one-shot from the Aboot prompt every time, so a power cycle returns to the
  vendor OS on its own. Making it the default is one line, and it should wait
  until the box forwards.

- **The kernel has no `tigon/tg357766.bin`**, so `tg3` logs a firmware load
  failure and disables EEE on the management port. The port works; only energy-
  efficient Ethernet is lost.

- **The management MAC is in `config/network.conf`.** `boot0` does not read
  prefdl on this board, so the one place that knows the address is a file
  correct for exactly one switch.

- **`aboot_max_hwepoch` is unset.** Left blank rather than guessed. Read it from
  `prefdl` under EOS and set it; Aboot refuses an image whose epoch claim is
  wrong, so a guess turns a working image into one that will not load.

- **The cooling band is carried, not validated.** 25–40 °C comes from the
  predecessor project running this board, translated from its five-step curve
  into NOSaic's band. Nothing has measured it here, and `fanread` returns
  garbage on the sibling board — whether it is trustworthy on this one is
  unestablished, so the thermal loop may be acting on numbers nobody has
  checked against this hardware.

- **Flash layout is provisional.** `boot_mib`/`slot_mib`/`data_mib` are carried
  from the SX2 and have not been sized against this board's 3.4 GB flash, which
  also holds two EOS images totalling around 1 GB.

- **Management MAC handling differs from the sibling.** On this board the real
  address is read in Aboot and passed on the kernel command line, because `tg3`
  comes up as the unprogrammed Broadcom default across a kexec. The SX2 reads it
  from a configuration file instead. Neither reads `prefdl` properly, which also
  gates the ASIC core voltage on that board.

- **The makefile's SDK-free build does not work, for either ASIC.** Both
  `datapath/td2/Makefile` and `datapath/td2p/Makefile` say `make` alone builds
  the BDE without the SDK, "which is what a contributor without the SDK staged
  can do and what CI runs". It fails at the first object: `main.c` includes
  `../common/l3sync.h` unconditionally and that pulls in `<bcm/types.h>`.
  Verified against `td2p` as well, so this is the tree's state rather than
  anything this board introduced. Either the include becomes conditional or the
  comment should stop claiming it — changing the comment is a claim about CI,
  so it is left to whoever owns that.

## Open questions

- **Which OpenBCM tree.** NOSaic builds `Salvaged-silicon/OpenBCM` 6.5.24; the
  reverse-engineering work links a different 6.5.24 checkout. The same version
  string is not the same tree, and the SDK's own compile-line defines have to
  match the library exactly or every shared struct is silently misaligned.

- **Whether the SCD reset bits differ.** The register *layout* is architecturally
  consistent across Arista platforms — the switch reset block is at `0x4000` on
  all of them — but the bit assignments vary per platform and this board's have
  not been established against the driver in this tree.
