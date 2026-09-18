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
      QSFP EEPROMs hang off the **ASIC's CMIC I²C**, not the board controller
      — the inverse of the Arista arrangement, and the easiest thing here to
      implement backwards.
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
