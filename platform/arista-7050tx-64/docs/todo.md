# Arista 7050TX-64 — what is left

Ordered by whether the switch works without it, not by effort. The board status
as a whole is in [the README](../README.md).

This port is at `bringup`: an image builds and carries a datapath, and nothing
has booted on the hardware. The split below is by what blocks what.

## Done

- **An image builds.** `make image BOARD=arista-7050tx-64` produces a 14.6 MiB
  SWI with a 63.1 MiB root filesystem, from the shared x86_64 toolchain and the
  `minimal` profile. Not booted.
- **`datapath/td2` and `recipes/nosd-td2` exist and build.** Derived from
  `td2p` — PCI device `0xb855` and revision `0x03`, both read off the board.
  The image picks it up and the "no datapath" warning is gone. Never run on
  hardware.

## Blocking — NOSaic does not run on this board without these

- **`cmicDevRevExpect` is hardcoded to Trident2+.**
  `internal/platformhal/scd/asic.go` expects `0x0002b860`, so the SCD driver will
  reject this board's CMIC. Making it per-ASIC is a **core** change: separate
  commit, its own reasoning, and worth asking whether the board should be
  supplying that value rather than the core knowing it.

- **The 48 BCM84848 PHYs have no driver in this tree.** Firmware must load over
  the SCD's MDIO before any copper port links, and neither existing board has an
  external PHY at all. This is the reason the port was deferred and it is the
  bulk of the work. The constraints that must survive into whatever is written
  are in [hardware.md](hardware.md#quirks) — they were each found the expensive
  way.

- **Nothing has been booted.** No toolchain run, no image, no console session
  with a NOSaic kernel on this hardware. Every claim in this directory is about
  the board, not about NOSaic on the board.

- **No flash backup since the board's contents changed.** The existing backup
  predates everything written to `/mnt/flash` since, and the vendor images on
  that flash are the recovery path. Take one before the first install, not after.

## Not blocking — the switch would run, short of these

- **`aboot_max_hwepoch` is unset.** Left blank rather than guessed. Read it from
  `prefdl` under EOS and set it; Aboot refuses an image whose epoch claim is
  wrong, so a guess turns a working image into one that will not load.

- **No thermal policy.** Deliberate, and the reason is a publication decision
  rather than missing information — see
  [the README](../README.md#an-open-question-before-this-goes-further). Until it
  is resolved the fans run at the controller's default rather than to a curve,
  which is safe but loud.

- **Flash layout is provisional.** `boot_mib`/`slot_mib`/`data_mib` are carried
  from the SX2 and have not been sized against this board's 3.4 GB flash, which
  also holds two EOS images totalling around 1 GB.

- **The port map generator is not written.** `tools/` should carry the
  equivalent of the SX2's `mkportmap.sh`/`mkpolarity.sh`, reading the map off a
  switch running the vendor OS. EdgeNOS has a working generator to adapt. Until
  then the datapath has no map to load and reports itself unconfigured — which
  is the correct failure, not a silent one.

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
