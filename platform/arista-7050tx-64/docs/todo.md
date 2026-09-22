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
- **ECMP was working and disclaimed.** The datapath had been building real
  `bcm_l3_egress_ecmp` groups all along; the capability response simply omitted
  the field, so `show caps` said `ecmp no` and an operator would have designed
  around a limitation the silicon does not have. Now reported from
  `bcm_l3_info`: `ecmp yes, up to 1024 paths`.
- **The chassis lamps were unreachable by mechanism, not by map.** They are
  32-bit words on the SCD here, not colour bits on the fan CPLD, so the driver
  sent SMBus bytes to a device that was not listening — and the error told
  operators to run the *sibling's* generator, which would not have helped.
- **The SMBus accelerator base was wrong**, so no module EEPROM on this board
  had ever been readable: accelerator 1 is at `0x9400`, not at the regular
  stride's `0x8080`.
- **The signal repeater was never driven.** A TI DS100KR800 in front of Et51 and
  Et52 was held in reset and then, once released, left unprogrammed. Its eight
  channels carry those two ports in the host-to-module direction only, so the
  cage received perfectly and transmitted nothing: this end reported the port up
  at 40000 and the far end reported it down. Programming it brought Et52 up,
  confirmed from both ends, with an OSPF adjacency and 1.8 ms round trip.

## Fixed on the hardware, 2026-09-15

- **It boots itself.** `boot-config` on the switch names NOSaic instead of the
  vendor OS, so a reboot needs no console and no Aboot prompt -- measured at 100
  seconds from `reboot` to ssh, and every reboot since has been hands-off. The
  EOS images stay on flash and Aboot still boots them on demand, so the way back
  is unchanged; only the default moved.

  ⚠⚠ AND IT SURVIVES A COLD POWER CUT, which is the bar this file set.

  PDU outlet 4 off, confirmed dark by ping and by the PDU, held 60 s, back on:
  ssh at 79 s and the datapath at 532 s, with no console, no Aboot prompt and
  nothing typed. Slot a still active, `boot-config` still NOSaic, all seven
  ports up, all three OSPFv2 adjacencies Full and 12 prefixes back in DEFIP.
  532 s against the 487 s of a warm reboot is PSU and firmware time, so nothing
  regressed. This board is now an installation rather than a demo.

- **The PHY firmware download was 13 minutes of sleeping, and is now 8.**
  `wait_response()` slept a flat 100 us before re-checking a transaction that
  completes in about 109 us, 3.66 million times. Spinning on the status word
  instead took boot-to-datapath from ~15.6 min to 487 s, reproduced twice.
  ⚠ The predecessor's own exponential back-off, ported exactly, measured SLOWER
  here (699 s) -- its `sal_usleep` and our `nanosleep` are not the same
  primitive at single-digit microseconds. Do not re-adopt it on authority.

- **A QSFP cage can be run as four 10G ports.** `config/portmode.conf`, applied
  before `bcm_attach` because there is no runtime switch for it on this chip.
  Proven against the empty cage so nothing cabled was at risk.

## Found by testing, and not yet fixed

- ⚠ **The fan curve has no headroom left, now that the copper PHYs run.** The
  band is 25-40 °C and the board sits above it: over one boot the hottest sensor
  read 42 °C on 1975 samples and 43 °C on 884, against 40 °C on 35 and 41 °C on
  68. The only duties ever produced were 95% and 100%, so the loop is working
  correctly and is pinned at the top, with nothing left for a real thermal event.

  This board's README predicted the mechanism before anyone measured it: "48
  copper PHYs dissipate considerably more than 48 SFP+ cages, and this board
  starts ramping ten degrees sooner". Until this week those PHYs were in reset.

  ⚠ It is NOT established whether the band is wrong or the board is genuinely
  this warm, and the difference matters. The band was carried from the
  predecessor rather than measured here, and the predecessor's own thresholds
  (25, 31.66, 36.66, 40) would peg at 42 °C too -- so "EdgeNOS ran the fans flat
  out as well" would not settle it. What would: EOS on this board with copper
  up. Full fans is the safe direction, so this is loud rather than dangerous.

- ⚠ **`tools/mkserdes.sh` tunes exactly one port, and the board has four cages.**
  It takes the FIRST tap profile it finds in the description file (`head -1`) and
  emits it for `PORT="${SERDES_PORT:-61}"`, so Et52 has transmit equalisation and
  Et49/Et50/Et51 have none at all -- no `serdes_preemphasis`, no
  `serdes_driver_current`. It was written during the Et52 bring-up and never
  generalised.

  It is recorded here rather than fixed because the obvious fix is unproven:
  applying port 61's profile to 49 and 53 by hand changed nothing, and the taps
  are per-PCB-trace tuning, so copying one cage's profile to another is an
  assumption. The description file does carry per-port descriptors; reading them
  properly needs the data file `/etc/prefdl` names, which is inside the vendor
  OS rather than beside it.

## Fixed on the hardware, 2026-09-14

- **The copper ports carry traffic.** Three faults, found by asking the
  predecessor what it did rather than by reasoning about ours.

  **Every port needs a linkscan mode.** `bcm_linkscan_mode_set_pbm` was called
  from the 40G bring-up only, so the four QSFP cages had one and the 48 copper
  ports did not. A port with no mode is missing from the link bitmap linkscan
  maintains, the transmit path ANDs its port bitmap with that one
  (`src/bcm/common/tx.c:5268`), and `_bcm_tx`'s `dv_vcnt == 0` branch frees the
  descriptor, logs a warning and returns `BCM_E_NONE`. Success, nothing sent.
  ⚠ That warning -- "Could not send pkt with dv_vcnt = 0" -- was in our own log
  51 times while this was being diagnosed from first principles.

  **The MAC interface must be written, not just matched.** `phy.c` read it back
  and wrote only on a mismatch. A 10G copper port wants XFI, XFI is the default,
  linkscan moves the MAC there on link-up, so nothing was ever written and the
  file produced no output at all. The predecessor writes it unconditionally.

  **Ports were never removed from VLAN 1**, so all 52 shared one broadcast
  domain. Invisible until copper could transmit, at which point the `et3`/`et4`
  patch closed a loop: 320 million frames each way and 1.2 billion flooded at an
  uninvolved 40G neighbour. Removed both when a tap builds its VLAN and,
  because the PHY download sits in between and that window alone leaked 23
  million frames, across the whole port bitmap the moment ports are enabled.

- **A lost DMA completion could mute the switch for ever, and every diagnostic
  would still say it was healthy.** The tap pump ran `bcm_tx` with no callback,
  which is the SDK's synchronous path: `async = pkt->call_back != NULL`
  (`src/bcm/common/tx.c:2680`) into `soc_dma_wait`, which is
  `soc_dma_wait_timeout(..., sal_sem_FOREVER)` (`src/soc/common/dma.c:4048`).
  The pump is the only thread that drains the taps, so one transmit that never
  completed parked the whole Linux-to-wire direction permanently.

  It happened to the sibling 7050SX2, which spent nearly two days hearing every
  neighbour and being heard by none while **this** board's `et49`/`et50` showed
  `link=1` and received nothing — so the fault presented here, on the wrong
  switch, and most of a day went into looking for it here.

  ⚠ **Nothing else failed.** Receive kept punting, counters updated, the query
  socket answered, `show ports` reported every port up at 40000, and `show dma`
  read 12% used with zero failed allocations — so the pool, the usual suspect,
  was demonstrably innocent. The one place the truth showed was the tap devices:
  `tx_packets` frozen while `tx_dropped` climbed at the Hello rate, which is
  what a TAP does when nobody reads the fd.

  Every packet now carries a callback, so the SDK takes `soc_dma_start` and
  `bcm_tx` returns without waiting for the wire. The single transmit buffer
  becomes a ring of 64 — a packet belongs to the DMA engine until its callback
  fires — allocated once, never freed, because the bump allocator still cannot
  free. A full ring drops and counts the frame rather than waiting. `tx-nobuf`
  on the port line and an explicit warning when the ring is exhausted exist so
  that degrading instead of stopping is something somebody is told about.

  Not fixed: why the completion went missing. The interrupt thread was alive
  and receive never faltered, so it is a lost wakeup rather than a dead IRQ.

## Tested on the hardware, 2026-09-13

A deliberate pass over the claims this board had not been asked to prove.

- **Unattended rollback of an unbootable slot.** The inactive slot was
  overwritten with random bytes and marked for trial. The boot did exactly what
  it says: `NOSAIC-BOOT-TRIAL slot b attempt 1 of 3`, then
  `NOSAIC-BOOT-ROLLBACK ... would not mount as squashfs; returning to a`, then
  up on the active slot with the trial cleared. It rolled back on attempt 1
  rather than spending all three, which is right — there is nothing to learn
  from remounting the same corrupt file twice.
- **The rollback leaves evidence.** `/mnt/data/boot/log` carried the decision
  afterwards, which is the point of writing it to the data partition: the slot
  files are cleaned up by a rollback, so the console is otherwise the only
  record and a switch in a rack has nobody watching it.
- **Configuration survives a rollback.** A marker written to
  `/mnt/data/config` was still there after the trial and the rollback.
- **Jumbo frames.** A 1572-byte payload — 1600 on the wire — crosses the fabric
  with no loss, so the MTU is real and not just configured.
- **IPv6 forwarding.** Eight OSPFv3 routes learned over `et52` and programmed
  into the chip.

- ⚠ **`nosaic platform tx <n> off` does not gate the laser on this board.** It
  writes the bit, reads it back changed (`0x108 -> 0x140`) and reports success,
  and the neighbour keeps receiving us: an adjacency held Full with an uptime of
  six hours across the whole test. The TX_DISABLE bit position is the sibling
  board's constant and is unconfirmed here, so the read-back check is verifying
  the wrong bit rather than the effect. A command that claims to turn a
  transmitter off and silently does nothing is worse than one that refuses.

  Consequence for testing: **link-down behaviour cannot be exercised from this
  end.** Flapping a port needs a cable pull or a far-end shutdown, so OSPF
  withdrawal, ECMP member removal and FIB reconvergence are all still unproven
  here.

- ⚠ **`nosaic verify ports` and `nosaic verify routes` are stubs.** Both are
  advertised in the CLI's own help and both answer "implemented in the C CLI and
  not yet here". They are the commands that compare what Linux believes against
  what the chip actually holds, which is exactly the check this board most
  wants — every FIB claim here rests on reading the datapath's own log instead.

## Blocking — the board is not at parity with the predecessor without these

- **Nothing has been ROUTED over a copper port.** Frames cross them now, both
  ways, but the proof used `et3` and `et4` patched to each other — and both ends
  being the same host is exactly what stops it going further: Linux answers no
  ARP for a request that arrives carrying its own address. So the datapath is
  proven and the protocol above it is not. **Either put a real neighbour on
  `et1`/`et2` and give it an address in `config/network.conf`, or put one end of
  the patch in a network namespace** — blocked on a neighbour or a namespace,
  not on code.

- **44 of the 48 copper ports are still untried.** All 48 answer `0x600d` and
  bind, so there is no reason to expect the rest to differ, but that is an
  inference and the other four are a measurement.

- **The watchdog is not armed, and arming it alone would be worse than leaving
  it.** Its action is a power cycle, so it needs a petting service to exist
  first; that service is the actual work. `nosaic platform watchdog arm <ms>`
  is there for a human who is watching.

- **No OSPFv3 adjacency with the 7050SX2.** There is one with the Edgecore on
  `et52`, and this end is configured and running on all three — `ospf6d`
  answers and every tap has a link-local address — so this looks like the
  neighbour rather than this board.

- **No flash backup since the board's contents changed.** The existing backup
  predates everything written to `/mnt/flash` since, and the vendor images on
  that flash are the recovery path. Take one.

## Not blocking — the switch runs, short of these

- **The management MAC and board identity still come from a file.** `prefdl` is
  on an i2c SEEPROM whose bus and address have not been established, so
  `nosaic platform status` cannot say what the board is and
  `config/network.conf` is the only thing that knows the address — correct for
  exactly one switch.

- **The PSU decode is the sibling's, and presence is not power.** Through the
  whole cold-cut test above -- outlet off, box dark, not answering ping -- this
  board's driver went on reporting `psu1? present` and `psu2? present` from
  register `0x00000003`. Those are presence bits for a module in a bay, and
  nothing here reads whether a supply is energised. A board fed from one outlet
  with two supplies fitted therefore looks fully redundant and is not, which is
  the wrong way round for a field to be wrong.

- **The cage-word decode table is the sibling's.**
  `internal/platformhal/scd/transceiver.go` knows `0x47`, `0x1c0` and `0x180`;
  this board reads `0x108` for a cabled cage and `0x105` for an empty one, so
  every cage decodes as "undetermined". Honest but useless, and read-only to
  fix: measure the four values here.

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
