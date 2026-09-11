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

## Blocking — NOSaic does not run on this board without these

- **Nothing has been booted, and this is now the only thing in the way.** An
  image builds and carries a datapath, a PHY layer and the board configuration,
  and not one line of it has executed on the switch. Every claim in this
  directory is about the board or about what the build produced — none is about
  NOSaic running on this hardware.

  What to expect when it does boot, so a first attempt is not read as failure:
  the four QSFP+ cages are direct SerDes and should come up first. The 48 copper
  ports depend on the SDK downloading PHY firmware, which fails transiently on
  some cold starts and is cleared by restarting the datapath. `phy:` lines in
  the log say whether the MAC interface is being matched, and silence from them
  with copper ports up is itself the signal that something is wrong.

- **No flash backup since the board's contents changed.** The existing backup
  predates everything written to `/mnt/flash` since, and the vendor images on
  that flash are the recovery path. Take one before the first install, not after.

## Not blocking — the switch would run, short of these

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
