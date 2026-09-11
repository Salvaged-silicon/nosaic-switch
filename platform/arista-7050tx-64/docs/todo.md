# Arista 7050TX-64 — what is left

Ordered by whether the switch works without it, not by effort. The board status
as a whole is in [the README](../README.md).

This port is at `bringup`: the board directory and the hardware documentation
exist, and no NOSaic image has been built or booted here. Everything below is
therefore outstanding — the split is by what blocks what.

## Blocking — NOSaic does not run on this board without these

- **`recipes/nosd-td2` and `datapath/td2` do not exist.** The board declares
  `asic: td2` and nothing provides `nosd` for it, so the image builder warns and
  produces an image with no datapath. Derive from `datapath/td2p/` — same CMICm
  generation, same architecture, same userspace BDE — reusing `datapath/common/`
  rather than forking it. Known deltas: PCI device `0xb855` not `0xb860`, and
  the external PHYs below.

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

- **Whether `iomem=relaxed` is needed here is unresolved.** The SX2 requires it
  for `/dev/mem` to map the DMA region; EdgeNOS does not pass it on this board
  and its BDE works, because it reaches BAR0 through `sysfs` instead. Which path
  NOSaic's `dmapool` takes decides it — read the code rather than copying either
  board's line.

## Open questions

- **Which OpenBCM tree.** NOSaic builds `Salvaged-silicon/OpenBCM` 6.5.24; the
  reverse-engineering work links a different 6.5.24 checkout. The same version
  string is not the same tree, and the SDK's own compile-line defines have to
  match the library exactly or every shared struct is silently misaligned.

- **Whether the SCD reset bits differ.** The register *layout* is architecturally
  consistent across Arista platforms — the switch reset block is at `0x4000` on
  all of them — but the bit assignments vary per platform and this board's have
  not been established against the driver in this tree.
