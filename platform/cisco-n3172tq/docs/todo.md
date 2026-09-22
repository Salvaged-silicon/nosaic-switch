# Cisco Nexus 3172TQ — what is left, in the order it has to happen

The reference for this board is the **Arista 7050TX-64**: same ASIC generation
(Trident II), same 48 × 10GBASE-T + 6 × 40G shape, same `datapath/td2`, and the
same BCM84848 copper PHYs. Where this page says "the reference does X", that is
the board to copy from — its `config/` and its `docs/todo.md` are the model for
this one. The 7050SX2-72Q is td2**p** and is only worth reading for the shape of
its `walkthrough.md`, which is the one rack-to-forwarding document in the tree.

Status as of 2026-09-18: **the board boots, cools itself and its datapath is
up.** It is not yet a switch, because nothing is on the Linux stack.

## What is proven on the hardware

Measured, not inferred. Everything below has been seen on the lab chassis.

- **Boots to userspace.** Netboot of an `mknbi-linux` NBI through the vendor
  loader's own TFTP, with four loader defects worked around (see
  [hardware.md](hardware.md) and the RE repo's `loader_linux_handoff.md`).
  s6-rc, login and FRR all come up. `/proc/cmdline` is ours, via a shim
  patched into the 32-bit entry.
- **The datapath is up.** `nosd: the datapath is up on unit 0`, 54 ports, over
  two hours stable. DMA out of `memmap=64M$0xb0000000`; BAR0 memory decode
  enabled explicitly (the chip ships with it off).
- **The PHYs are configured.** 48 × `phy_84848` copper and 6 × `phy_84328`
  retimers loaded from `config/portmap.conf`; 6 × 40g from
  `config/portmode.conf`.
- **The port map is validated against a cable.** Panel ports 31 and 32 were
  patched to a 7050TX-64 and came up as **logical ports 31 and 32, UP at
  10000** — the first independent check that the generated map matches the
  physical panel.
- **Environmentals.** `adt7462` plus two `pmbus` bound at boot; three board
  diodes and the controller die read; four fans commanded by the thermal loop;
  PSU presence. See [hardware.md](hardware.md).

## Blocking — this is not a switch until these are done

Ordered so each step's failure is diagnosable with the one before it working.

- [x] ~~**No taps, so nothing is on the Linux stack.**~~ — **done, and the
      reference's policy was not copied.** All 54 ports are declared in
      `config/asic.conf`, not just the cabled two.

      The 7050TX-64 declares taps only for ports that have a cable in them,
      justified on cost: a tap on a dark port spends a VLAN, a router
      interface and a MY_STATION entry. True, and irrelevant — 54 of each on
      a Trident II is nothing. What the policy actually costs is that
      `asic.conf` is read once at `nosd` start, so a port with no tap is not
      an interface that is down, it is an interface that does not exist.
      Plugging a cable into it does nothing, shows nothing and logs nothing
      until somebody edits a file and restarts the datapath. The set of
      usable ports ends up decided at build time by whoever last wrote the
      config rather than by whoever is holding the cable.

      ⚠ **The reference should be changed to match**, not the other way round.

- [x] ~~**The management MAC is the unprogrammed NIC default.**~~ —
      **worked around, and the hardware turned out to carry it after all.**

      `eth0` comes up as `00:a0:c9:00:00:00`, Intel's OUI with an all-zero
      suffix, i.e. a blank NIC EEPROM. `config/network.conf` now states the
      real address.

      Unlike the reference, it was not copied out of the vendor OS — it was
      **read from this board's own ID PROM**, the `24c512` at `0x52` that the
      i2c service already instantiates. The layout, measured:

      | | |
      |---|---|
      | Two records | `0x0000` board, `0x1000` chassis, 4096 bytes each |
      | `+0` | magic `ab ab` |
      | `+14` | `Cisco Systems, Inc.` |
      | `+34` | product ID (`N3K-C3172TQ-10GT`) |
      | `+54` | serial (`FOC22010NYL` board, `FOC2201R1WZ` chassis) |
      | `+74` | part number (`73-15384-02`, `68-4949-01`) |
      | `+90` | revision (`R0`, `S0`) |
      | `+184` | **MAC base** — `b4:de:31:3f:a5:c0`, chassis record only |
      | `+190` | MAC count, `0x0080` = 128 addresses |

      - [x] ~~**Read it in the HAL instead of the file.**~~ — done for the
            identity half. `internal/platformhal/n3172tq/idprom.go` decodes
            the PROM and `Board()` reports the chassis record, so `identity`
            no longer says `ErrUnsupported`. The at24 is found through the
            declared mux channel rather than a hardcoded bus number, a record
            without the `0xabab` magic is refused outright, and a field that
            reads as unprintable is dropped rather than reported — a
            plausible wrong serial being worse than none. Tested against the
            measured bytes of both records.
      - [ ] **Use the PROM's MAC instead of `network.conf`.** The decoder
            already returns it; what is missing is a path from the HAL to the
            management interface, since `Identity` has no MAC field and the
            network service reads a file. ⚠ Offsets are still two records
            from one unit — check against a second 3172TQ before trusting
            them generally.

- [x] ~~**No `config/network.conf`.**~~ — written. Management `10.10.39.3/24`
      (NX-OS keeps `.2`), default route, a route to the build/TFTP host
      network, loopback `10.101.255.54/32`, and a `/29` per transit link:
      `eth1_31` `10.101.101.89`, `eth1_32` `10.101.101.97`. Gateway
      `10.10.39.1` was confirmed reachable before choosing it.

- [x] ~~**`net_wait_secs: 60` would have thrown the front-panel addresses
      away.**~~ — removed, so it takes the 1500 s default. The 60 was correct
      when no port map existed and the taps could never appear; with the map
      generated it means `eth0` is configured, every front-panel address is
      silently skipped, and the box comes up with no transit addressing —
      which reads as a dead data plane rather than a timeout. This board has
      48 external PHYs whose firmware is downloaded over MDIO before the chip
      reports its ports, so the wait is minutes.

- [x] ~~**The datapath silently truncated the tap list at 8.**~~ — found by
      declaring all 54, and fixed in `datapath/`.

      `datapath/common/tapbridge.c` deliberately *refuses* rather than
      truncates, on the reasoning that dropping taps quietly produces a switch
      short some ports for no stated reason. That protection was dead code:
      both `datapath/td2/main.c` and `datapath/td2p/main.c` declared
      `struct tap_spec specs[8]` and stopped scanning at `ntap < 8`, so the
      caller truncated before the callee ever saw the count. Declaring 54 gave
      8 taps, `eth1_1`..`eth1_8`, with nothing said — and the network service
      then waited out its whole deadline for interfaces that were never going
      to exist. The old `contract 1, ports 8 max` recorded for the 7050TX-64
      was this cap, not a property of the board.

      The bridge's limit is now `NOSAIC_MAX_TAPS` in `tapbridge.h`, both
      callers are sized to it, and a board declaring more than that is
      refused with a message naming the count. ⚠ This is a shared-datapath
      fix: it changes the 7050TX-64 and 7050SX2 too.

- [x] ~~**Transmits on dark ports were counted as successes.**~~ — the second
      thing declaring all 54 ports exposed, also fixed in `datapath/`.

      `bcm_tx` ANDs the packet's port bitmap with the bitmap linkscan
      maintains, so a port with no link yields no descriptor: the SDK prints
      `Could not send pkt with dv_vcnt = 0`, invokes the completion callback
      inline, **and returns success**. Linux sends router solicitations and
      MLD out of every interface it has, so 52 dark ports produced a steady
      drip of failed transmits recorded in `tx_ok` as though they had gone
      out — 364 of them in the first few minutes.

      `tap_tx()` now checks link before handing the frame over and counts
      `tx_nolink` instead. Measured after the fix: **0** `dv_vcnt` messages,
      and dark ports report `tx-ok=0 tx-nolink=N`. ⚠ Shared-datapath fix;
      it changes the other td2/td2p boards too.

- [x] ~~**Nothing has been forwarded or routed.**~~ — **both links carry
      traffic and OSPF is Full on both**, 2026-09-18. `10.101.101.33` and
      `.97` here against `.34` and `.98` on the 7050TX-64, 0% loss, two
      adjacencies to `10.101.255.50`, and the Nexus loopback reachable from
      the Arista over an **ECMP pair** (equal cost on both links). This is
      the first traffic either board has passed to a neighbour.

- [x] ~~**Routes were learned but never reached the chip.**~~ — the third
      hardcoded 8, and the one that actually mattered.

      With full adjacencies and 18 OSPF routes in the RIB, the chip held
      **none**: `l3: CHIP route 0/15360  intf 9/8192`, with every route
      counted under `not a router interface`. The cause was
      `#define MAX_IF 8` in `datapath/common/l3sync.c`:
      `nosaic_l3_add_intf()` returned -1 past the eighth interface and its
      caller in `td2/main.c` ignored the result, so taps 9 onward got no
      router interface. `eth1_31` and `eth1_32` are taps 31 and 32.

      It hid well. The one printf that names the interface is rate-limited
      to four lines, and all four were spent on `eth0` — which legitimately
      has no router interface, being the management NIC. So the log said
      exactly one true and completely misleading thing.

      `MAX_IF` is now `NOSAIC_MAX_TAPS`, the overflow says which interface it
      refused, and the caller reports the failure. Measured after:
      `CHIP route 15/15360  intf 55/8192` — 54 taps plus one, and routes in
      the silicon.

- [ ] **The 7050TX-64 needs this same fix before two-hop traffic works.**
      Its `nosd` now knows 52 taps and it is running the old binary:
      `intf 9/8192`, so 44 of its ports have no router interface. The
      symptom from here is that `10.101.255.50` (one hop) answers while
      `10.101.255.53` and `10.101.101.241` (two hops, transiting the Arista)
      are 100% loss — even though the far boxes have correct return routes.
      It needs a rebuild with the current `datapath/`.
 Both linked ports counted
      `rx 0 pkts / tx 0 pkts` over 25 s, which is correct for two unconfigured
      ends. **This end is now configured** — `eth1_31 10.101.101.89/29` and
      `eth1_32 10.101.101.97/29`, verified applied on the hardware. What
      remains is the far end: the 7050TX-64 has taps for `et49/et50/et52` and
      `et1`..`et5` only, so its ports 31 and 32 are not on its Linux stack.
      Add the matching pair there (`10.101.101.90` and `.98`) and this is the
      first real traffic test either board has had. The reference is blocked on exactly this and for exactly one
      reason — no neighbour. **That reason is now gone:** ports 31/32 are
      patched to the 7050TX-64, so both ends are NOSaic boards we control.
      Configure both ends and this becomes the first real traffic test on
      either board.

- [ ] **It is not installed.** Every boot is a netboot; a power cycle returns
      the box to NX-OS. That is the safety property and it is deliberate, but
      until it changes this is a demo.

      ⚠ **The install path is not the path being exercised.** Development
      netboots through the vendor loader's NBI container; the installer writes
      an EFI system partition and the firmware boots the kernel directly via
      `CONFIG_EFI_STUB`. Those are two different handoffs and only the first
      has ever run. Do not assume the install works because netboot does.

- [ ] **Recovery has never been exercised.** There is exactly one copy of
      `n3100-compact.7.0.3.I7.9.bin` on the chassis. Save it off and verify
      its md5 *before* installing, then prove the way back by netbooting it
      and running `install all`. Recovery that has never been run is not
      recovery.

## Not blocking — the board runs, short of these

- [ ] **The front panel is dark.** `led: no SCD found; the front panel stays
      dark`. Chassis LEDs are 32-bit MMIO over the PLX PCI9030's local-bus
      windows and the offsets are unknown; port LEDs belong to the ASIC. The
      reference drives both through its SCD, which this board does not have,
      so this is new work rather than a port.
- [ ] **Transceivers and cages are not wired.** `PlatformHAL.Cages` is nil and
      the `retimer`/`transceivers` services are gated off. ⚠ On this board the
      QSFP EEPROMs do **not** hang off the ASIC's CMIC I²C, despite what
      `docs/hardware.md` says — measured under NX-OS with a module fitted
      and readable by the vendor OS, `bcm-shell.0> i2c probe` answers
      `I²C: detected 0 devices`. They are on the platform SMBus like
      everything else, and a scan of every mux channel found no `0x50`
      either — which fits, because a QSFP only answers i²c while its
      `ModSelL` is asserted and nothing asserts it. That makes the PCA9539s
      — the inverse of the Arista arrangement, and the easiest thing here to
      implement backwards.
- [x] **PROVEN END TO END: 40G carries traffic.** `eth1_54` to the 7050SX2's
      `et49` holds a Full OSPF adjacency and pings 4/4 at 0.55 ms average,
      with counters moving in both directions. That exercises the whole
      chain -- retimer enable, firmware download, lane maps, polarity and
      tuning -- so the datapath side of the cages is finished, not merely
      linking.

      ⚠ `eth1_53` IS PATCHED TO THE AS5610's `swp50`, NOT `swp49`, and
      swp50 is not configured on that box at all -- absent from its
      network.conf and frr.conf, and not even listed by `nosaic show ports`.
      Its config expects us on swp49 (`# swp49 is the uplink to the Cisco
      Nexus`, `iface swp49 10.101.101.18/29`), so the cable and the
      configuration disagree. Either move the fibre to swp49 or configure
      swp50; nothing on our side needs to change.

      How to identify which far-end cage a fibre lands in, without touching
      the rack: disable our transmitter (`show phywrite <port> 1 0x09 1`) and
      read the QSFP LOS byte on every candidate cage at the far end -- byte 3
      of page 0, read TWICE because it latches. Exactly one goes to 0x0f. Our
      eth1_53 lights bus 67, which is cage 1, which is swp50. Restoring the
      transmitter clears it. The reverse check is also worth knowing: the
      far end's LINK STATE is not a reliable witness (the AS5610's swp49 held
      `up` throughout with its own laser disabled), but LOS at the module is.

      Superseded reading, kept because it was confident and wrong: this was
      recorded as one-way with the AS5610's `swp49`, on the strength of its
      in-nuc advancing at our OSPF hello rate. That was coincidence --
      swp49's own background traffic -- and swp49 is patched to something
      else entirely.

- [x] **SOLVED: no 40G cage linked, and it was one bit in the retimer.**
      `1.0xc8e4` bit 15 -- the BCM84328 powers up with it clear, NX-OS
      sets it, we did not. Set at the end of cage bring-up now; both
      cages come up at 40000 from a cold boot. The history below is
      kept because two confident readings of the evidence were wrong.

- [x] **⚠ NO 40G CAGE RECEIVES UNDER NOSaic, AND IT IS OUR SOFTWARE.**
      This overturns the earlier conclusion in this file that the dark cage
      was physical and needed a module swap. It is not.

      The decisive contrast: panel 53 to the Edgecore AS5610's `swp49`
      **links at 40G under NX-OS** — same cage, same module, same fibre,
      same far end — and reports `link=0` under NOSaic minutes later.

      Every 40G combination tried shows the same one-way result, and the far
      end always sees us:

      | our cage | far end | far end sees us | we see them |
      |---|---|---|---|
      | 54 | 7050SX2 `et49` | yes | no |
      | 53 | 7050SX2 `et50` | yes | no |
      | 53 | AS5610 `swp49` | yes | no |

      Two of our cages, two modules, three far-end ports on two different
      switches. Our transmit always works; our receive never does. That rules
      out optics, fibre and far ends, all of which the earlier note blamed.

      Our port configuration is not the difference either — both cages read
      `interface 28 is 40G already, left alone` and `0 setting(s) applied`,
      which is exactly the state NX-OS runs them in (`ps xe68`: SR4, enabled,
      autoneg off, 40G FD).

      **The remaining difference is the BCM84328 retimer.** NX-OS's SDK
      enumerates it at MDIO `0x54`; our datapath never mentions an 84328
      beyond reading `phy_84328_<port>` as a property name. A retimer whose
      line side was never brought up is exactly a port that transmits and
      does not receive. The copper ports hide this because a BCM84848
      autonegotiates unaided.

      ⚠ Note what misled us: the very first cage tried also had a genuinely
      suspect module in it, and NX-OS failed on that one link too, which
      read as confirmation that the fault was physical. One bad module in
      the first sample sent the whole investigation to the rack. Test
      against a second far end before concluding it is hardware.

      **Update — the retimer had no firmware, and that is now fixed.** It
      was not "never brought up" in the sense above: the driver bound and
      configured it correctly. It had no microcode. `phy_force_firmware_load`
      was `0` in `config/asic.conf`, which property.h documents as MAY SKIP
      LOAD, and the SDK took the permission -- the framework entered the
      broadcast download and returned without calling one driver step. At
      `0x11` all six report `PHY84328 Firmware revID=0x29`. The cages still
      do not link, so this was a precondition rather than the fault.

      **The ASIC-to-PHY path is now PROVEN GOOD and is not the fault.**
      `nosaic show loopback 65 2` -- PHY local loopback -- brings the port
      **up at 40000**. That exercises the MAC, the PCS, all four SerDes
      lanes, the lane maps and the polarity in both directions. MAC loopback
      links at 10000 and no loopback is down. So everything from the MAC to
      the PHY's line interface works, and the fault is on the line side.

      ⚠ **THE "FAR END ALWAYS SEES US" COLUMN ABOVE DID NOT SURVIVE A DIRECT
      TEST, AND THE TABLE SHOULD NOT BE TRUSTED.** It was read off far-end
      link state, and a far end's link bit says its RECEIVER locked -- which
      on the AS5610 turns out to be latched anyway: `swp49` stayed `up` at
      40000 with our own PMD transmitter explicitly disabled
      (`phy.write 65 1 0x09 1`, read back as 1). Worse, its receive counter
      kept advancing at exactly the same rate with our laser off as with it
      on -- +16 per 100s either way -- so `swp49` is not being fed by our
      cage at all and never was. Every inference of the form "our transmit
      works, only our receive is broken" rests on that column. Re-establish
      it with counters and a laser toggle before building on it again.

- [x] **The QSFP cage GPIO correlation is DONE -- and it was a dead end.**
      Recorded because a dead end that cost a day is worth one that
      costs nobody another. The three lines NX-OS drives and we left
      floating are now driven identically here, the expanders read the
      same on both systems, and the cages stayed dark. The real fault
      was `1.0xc8e4` bit 15, above.

      ⚠ The reverse test -- reverting the lines under NX-OS, which did
      not drop its links -- proved less than it looked like: the
      modules were already initialised. Only the forward test counts.

- [x] **The correlation method itself, which is the reusable part.**
      board.yml prescribed it: read the three PCA9539s under NOSaic, boot
      NX-OS which drives these cages successfully, read the same registers,
      diff. Both halves are now measured (registers 0x00-0x07, in0 in1 out0
      out1 pol0 pol1 cfg0 cfg1):

      | device | NOSaic | NX-OS |
      |---|---|---|
      | bus6/0x74 | `f0 80 ff 80 00 00 ff 7f` | `f3 80 ff 80 00 00 fc 00` |
      | bus6/0x76 | `83 fe 83 fe 00 00 00 00` | `83 fe 83 fe 00 00 00 00` |
      | bus2/0x75 | `7f fa ff fa 00 00 ff 00` | `ff fa ff fa 00 00 1f 00` |

      ⚠ **0x76 is byte-for-byte identical under both**, so board.yml's
      reading of `port 0 = 0x83` as "five cages held in reset" is WRONG --
      whatever those bits are, the working OS leaves them exactly where we
      do. Delete that hypothesis rather than carrying it.

      What actually differs is the direction registers, and only three lines
      change LEVEL. NX-OS makes these outputs and drives them HIGH where we
      leave them as floating inputs reading LOW:

        * 0x74 port 0 bit 0
        * 0x74 port 0 bit 1
        * 0x75 port 0 bit 7

      (0x74 port 1 bits 0-6 also go from floating to driven, but NX-OS
      drives them to the level they already float at, so they are a
      direction change without a level change.)

      Three lines against six cages is not a per-cage signal, so this is not
      ResetL. Next step is to drive those three as NX-OS does and see
      whether `pma.sigdet` on a cabled cage stops reading zero. That is a
      poke at a mapped bit rather than at an unmapped expander, which is
      what board.yml's warning was actually about.

      Read them with `nosaic platform i2c <bus> <addr> <reg> [count]`, which
      is read-only on purpose; under NX-OS, `feature bash-shell`, then
      `sudo mknod /dev/i2c-0 c 89 0` because NX-OS has i2cget but ships no
      device node, and its single adapter is the root SMBus with the muxes
      driven in its own code rather than by the kernel. 0x70 selects the
      channel (0x02 = channel 1 = kernel i2c-2, 0x20 = channel 5 = i2c-6);
      it sits at 0x02, so 0x75 reads with no mux write at all. Put it back.

- [ ] **Breakout is unproven.** All six cages are declared `40g`; none has
      been broken out to 4 × 10G.
- [x] ~~**No `config/frr.conf`.**~~ — written: router-id from the loopback,
      OSPFv2 and OSPFv3 on both transit links at **equal cost**, so the pair
      is an ECMP pair rather than a primary and a spare. `max-metric
      router-lsa` is set, so the box is reachable but nothing routes through
      it until the datapath has earned the traffic.

      Two things this fixed that were not obvious:
      - **Without it the board ran another switch's configuration.** The frr
        package's default `frr.conf` is not neutral — it carries router-id
        `10.101.255.53` and that board's networks. Two routers claiming one
        id is a fault that presents as a flapping neighbour.
      - **`can't open logfile /var/log/frr.log`** on every boot: the daemons
        drop to the `frr` account and /var/log is not theirs to write. The
        7050TX-64 avoids it with `log syslog`, which is no better here —
        ⚠ **no profile in this tree ships a syslog daemon**, so those go
        nowhere at all, quietly. This board uses `log stdout`.

- [ ] **Fix the frr package default for every board.** Shipping one board's
      router-id as the fallback, and a log path the daemons cannot write, are
      both wrong independently of this port. Left alone here because it
      changes every board and the 7050TX-64 is mid-configuration.
- [ ] **Identity, watchdog and reset lines report `ErrUnsupported`**, each
      honestly: the PROM layout is the vendor's and undecoded, and no watchdog
      or reset line has been found to be reachable.
- [ ] **ACL is not wired for td2 here.** `feature/acl-td2` exists and is
      untested. This chip's ingress FP is 4096 entries, twice Trident+'s; the
      prediction on record is ~2560 v4 + ~1536 v6, expected to come out lower.
      Record whichever way it falls.
- [x] ~~**The ASIC die temperature is invisible to the thermal loop.**~~ —
      it reads, and it is the hottest thing in the box by 14 °C.

      The die is reachable only through the SDK over PCIe, so the only
      process that can ask is `nosd`. The datapath serves it as an
      `asic.temp` query (`bcm_switch_temperature_monitor_get`, converted from
      the SDK's 0.1 °C to millidegrees once, at the source), and this board's
      HAL adds it to `Temperatures()`. `internal/thermal` needed no change:
      the reading simply arrives and is taken as the hottest, which is what
      that package's comment said would happen.

      Degradation is deliberate and tested: no datapath running, a datapath
      too old to answer, a chip with no monitors, and a monitor reading zero
      are all "no reading" rather than "0 °C" — zero on the hottest part of
      the board would wind the fans *down*. A wedged `nosd` costs one
      interval, not the cooling loop, via a deadline on the query.

      Measured: `temp ASIC 49.3 °C` against 30.2–35.0 for the board diodes.

- [ ] **`thermal` takes one band against the hottest sensor, and this board
      has sensors whose trip points differ by more than two to one.** The die
      trips at 100, the Back diode at 46. Keyed to the die (55–90, which is
      what `board.yml` now sets), Back could reach its own minor trip with
      the fans still at the floor. Today that is covered by a measured
      offset — the die runs ~19 °C above Back, so Back at 46 implies a die
      around 65 — but that is an inference from one set of readings and it
      fails for anything that heats the chassis without heating the ASIC: a
      failed fan, a blocked intake, a hot PSU. The fix is per-sensor
      thresholds in `internal/thermal`, and it is shared code affecting every
      board.
      ⚠ The first band shipped with the die in the loop was 38–52, keyed to
      the diodes, and it ran the fans at **83% on an idle switch** where the
      vendor runs 16%. If a band ever looks absurd, check which sensor is
      driving it before adjusting the numbers.

- [ ] **Adopt `tap_mac_base`, and feed it from the ID PROM.** The tap MACs
      are generated identically on every board — `02:00:00:00:00:50` upward
      by tap index — so this board and the 7050TX-64 hold overlapping ranges
      (`0x50`–`0x85` against `0x50`–`0x86`) and collide index for index: our
      `eth1_1` and its `et49` are both `02:00:00:00:00:50`. Measured, both
      boards, 2026-09-18. The cabled pair escapes only because the same index
      lands on different ports — their tap list starts `et49/et50/et52` — so
      this is luck, and it changes the next time either tap list does.

      `tapbridge.c` takes a `tap_mac_base` property now; setting one here
      fixes it. Better: **this board does not need a made-up base.** Its ID
      PROM carries a real allocated block — `b4:de:31:3f:a5:c0` with a count
      of `0x0080`, 128 addresses for 54 taps — which is what a MAC block is
      for, and `idprom.go` already decodes it. A locally-administered
      constant is the right default for a board that cannot tell you; it is
      the wrong answer for one that can.

- [ ] **No hardware cooling backstop.** The ADT7462 can run the whole curve
      itself, but its auto registers are factory defaults (no sensor routed to
      any fan, ramp starting at 90 °C) and Cisco did not use them either. The
      software loop already sets fans to 100% on any clean exit, so this only
      covers `SIGKILL` and a wedged kernel. Cheap to add; does nothing for the
      die blind spot.

## Open questions

- **Why the kernel hangs early with `CONFIG_EFI_MIXED=n`.** Established by
  bisect, not root-caused. Left at the default, with the reasoning recorded in
  `recipes/linux/config/x86_64.fragment`.
- **Whether the EFI-stub install path hits the same wall the NBI path did.**
  Unknown until an install is attempted; if it fails identically, the loader
  question and the firmware question turn out to be one.
