# Arista DCS-7150S-52 — what is left

Everything. This is a `planned` board: no NOSaic image has been built for it and
no NOSaic code has touched the chip.

Ordered by what blocks what. Each step is a thing that can be demonstrated, and
a step is not done because the code for it exists.

## The rule this board is being ported under

**No replay, no warm-inherit.** The FM6000 is brought up from a cold chip by code
that knows what it is writing and why. The two shortcuts are both ruled out:

- **warm-inherit** — boot NOSaic from a running EOS without a power cycle so the
  chip keeps EOS's configuration. Proves a byte-mover; produces a switch that
  needs the vendor OS to start, which is the opposite of the point.
- **register replay** — play a captured EOS bring-up trace at the chip. It can be
  made to forward packets. It cannot take a port map, cannot be told to bring up
  a different set of ports, cannot be explained, and carries the licence of the
  capture.

A captured trace is still the best available *oracle* — the ground truth for what
a configured FM6000 looks like. Reading our own chip state back and diffing it
against one is right; emitting one is not.

The line that decides it is **redistribution**. NOSaic has to stay a repository
anyone can take and an image anyone can publish, so no vendor artifact goes in
it — not a trace, not a blob, not a table lifted out of one. That rules out
shipping Arista's parser microcode even under a non-redistributable recipe: this
board generates its own (see
[hardware.md](hardware.md#the-parser-is-microcoded-and-we-write-the-microcode)).

Knowledge is not an artifact. Reading Intel's datasheet and implementing what it
documents is how every driver is written; a datasheet is fetched rather than
committed, as on every other board.

## M0 — the box boots NOSaic

- [x] **flash space.** 511 MB free as of 2026-09-22, up from 59 MB: twenty-five
      old EdgeNOS builds removed. EOS stays. About 530 MB more is reclaimable
      from RE traces if two slots will not fit. See
      [hardware.md](hardware.md#flash-space)
- [x] **the image builds.** Minimal x86_64, 2026-09-23: rootfs 43.4 MiB, kernel
      13.6 MiB, installable SWI 14.8 MiB, RAM-boot SWI 58 MiB. `nosd-fm6000` is
      in it, so `asic: fm6000` resolves to its provider
- [x] **a RAM-bootable SWI exists.** The aboot backend had no `Netboot`, so
      `--ram-boot` produced a kernel and an initramfs and nothing Aboot could be
      pointed at. It does now, and the bundle ships a README
- [x] **a NOSaic kernel boots on this chassis** — RAM-booted 2026-09-23 to a
      `nosaic login:` prompt. See
      [hardware.md](hardware.md#nosaic-booted-on-this-switch-2026-09-23)
- [x] **`config/network.conf` added** so it comes up with its management address
      and real MAC. (An earlier note claimed the image could not be logged into;
      that was wrong — `root` is locked by design, the account is `admin`, no
      password, console only)
- [x] **`nosd` no longer restart-storms** — it waits up to 30s for the chip
      (`--wait`) then exits non-zero, keeping the A/B semantics
- [x] **`boot0` dry-run bug fixed** — the `ma1` cycle ran before the `testonly`
      exit, so a dry run left the interface down and broke the next netboot
- [x] **installed on flash and boots NOSaic by default, 2026-09-25.**
      `/mnt/flash/NOSaic.swi` with `boot-config` → `SWI=flash:/NOSaic.swi`. The
      EOS image stays (436 MB, 450 MB still free) and reverting is one line —
      `boot-config.eos` next to it holds the original. NOSaic can mount
      `/dev/sda1` itself, so it updates its own flash image and the vendor OS
      is no longer needed for anything.
- [x] **network login works** — `config/authorized_keys` installed. ⚠ The keys
      land on **root**, not the login account: dropbear refuses any account
      whose password field is blank before it looks at a key, and `admin` has
      none. So `ssh root@`, not `ssh admin@`.
- [ ] **the SSH host key does not persist across reboots.** The rootfs is a
      read-only squashfs, so dropbear regenerates into tmpfs every boot and
      every connection trips `REMOTE HOST IDENTIFICATION HAS CHANGED`. Harmless
      in a lab and wrong in a product — it trains operators to ignore the one
      warning that matters. The key belongs on the data partition.
- [ ] ~~`config/authorized_keys` for network login~~ — gitignored and per-operator,
      so created on the machine that builds, not committed
- [x] **`boot_mib` / `slot_mib` / `data_mib` set from a measured image** —
      64/128/128. A slot's content is 57 MiB (kernel 13.6 + rootfs 43.4), and
      the layout fits 511 MB free with room to double. The defaults would have
      given a 96 MiB slot, which is 39 MiB of headroom on this image.
- [ ] **the management NIC does not come up yet.** `tg3: No PHY devices` — the
      BCM50610 never answers on MDIO. Two causes found and both fixed in the
      tree, neither yet confirmed on hardware: `CONFIG_BROADCOM_PHY` was missing
      from the x86_64 kernel fragment, and the kernel disables the PHY's
      internal RGMII delays for the only interface mode tg3 accepts. See
      `recipes/linux/patches/0003-...` and
      [hardware.md](hardware.md#0-the-management-nic-does-not-come-up)
- [ ] `eth0` comes up with the board's real MAC, read from prefdl rather than
      configured — `MacAddrBase 44:4c:a8:31:5d:aa` on this chassis, and the
      prefdl parser is shared work with the 7050SX2, which has the same gap
- [ ] choose the reserved DMA region and prove the kernel survives it — the
      address depends on where this board's ~3978 MB ends
- [x] **recovery without the PDU is demonstrated** — the SCD watchdog
      power-cycles the board. Armed for 60 s from a RAM-booted NOSaic and left
      unpetted, it brought the box back on EOS, pingable 110 s later. Arm it
      before touching the chip.
- [ ] **find out whether this board can reboot.** EOS kexecs and does not trust
      the hardware reset; every reset path tried from a bare kernel has hung.
      Until this is answered, A/B upgrade on this board is a claim with nothing
      under it.

## M1 — the ASIC is reachable  ✅ DONE 2026-09-25

**This milestone was named wrong and that is why it took so long.** It was
"the ASIC is on the bus", and the ASIC never needs to be on the PCI bus to be
reachable. The FM6000's registers are mapped into the **SCD's BAR1**, a
PCI-to-LocalBus window that is live from power-on. See the section at the top
of [hardware.md](hardware.md).

- [x] **register access on a cold board, with the FM6000 absent from `lspci`** —
      `PIN_STRAP` reads `0x00000208` through SCD BAR1, matching what a
      forwarding chip reads. Four registers cross-checked warm against PCIe and
      all four agree exactly.
- [x] **the access sequence is a reset PULSE** — assert `0x106` to the SCD's
      `resetSet` (`BAR0+0x4000`), then clear `0x106` to `resetClear` (`+0x4010`).
      Resets merely left clear from boot give a silent chip that reads `0`
      everywhere. This is what made the earlier release experiments look like
      failures.
- [x] **the datapath can use it** — `fm_open_lbus()` in `datapath/fm6000/pci.c`,
      `fm6000-probe --lbus`.
- [x] **the hazard is measured, not just guarded in the abstract** — reading any
      of the EPL block (`0x0e3000`–`0x0e4fff`) before init takes the chip off
      the local bus; `fm_hazard()` refuses it. A dead chip reads `0x00000000`
      here, **not** `0xffffffff`.
- [x] **the release pulses** — `switch_reset_always_pulse: true`. It used to
      take a shortcut when nothing read held, so on this board the release was
      a no-op that reported success. Verified on hardware: the trace reads
      `0x106 → 0x104 → 0x100 → 0x000`, and the chip answers afterwards.
- [x] **the release no longer waits for PCIe** — `switch_pcie_after_config:
      true`. It waited for an endpoint that cannot appear until the datapath
      has configured the chip, timed out after a perfectly successful release,
      and reported a working board as broken. `release-asic` now exits 0 and
      says what actually happened.
- [ ] **PCIe enumeration is an M4/M5 question.** The chip is expected to appear
      on the bus only after it has been configured — that is what the vendor
      agent does, switching from `localBusHam` to `pciHam` at the end of
      bring-up. Nothing needs it before then, because packet DMA is the only
      thing that uses it.

### What the vendor agent tells us about the bring-up to come

From `/var/log/agents/FocalPointV2-3350`, for M3/M4. **live**

- `Ring mode set to 5 (52 ports, 64 tokens, 4 locked, 4 slow, 1 sync)`
- `Using microcode init func fm6000UcLibraryInit` — the parser microcode is
  loaded from a library, which is the thing we generate ourselves.
- `Chip version is B2 ( 64 ports )`
- `SERDES lanes are ready in 13836 usec. PollCnt 109. PCI_IP = 0x20` — SerDes
  readiness is polled, takes ~14 ms, and `PCI_IP` is read *after* it.
- a 64-entry `SSCHED_INIT_TOKEN` scheduler table, `RX=`/`TX=` per token.
- the full physical→logical port map, now in
  [hardware.md](hardware.md#the-port-map-recovered-from-the-vendor-agent).

### The original M1 investigation, superseded

Kept because its measurements stand even though its conclusion does not: the
SCD register map, the reset bit names, the SMBus map and the device
identifications all came out of it and are all still correct.


- [x] **the SCD reset block is mapped and works** — measured from Aboot
      2026-09-23. Cold it reads `0x106` (bits 1, 2, 8 held); the clear port at
      `0x4010` drove it to `0x000` in three writes. Unimplemented bits read as
      **zero** here, the opposite of the 7050SX2
- [ ] **find out what else NorCal init does.** With every reset bit cleared the
      FM6000 still does not enumerate, and a bridge rescan does not find it
      either. Leading hypothesis is the **ASIC core rails** — prefdl carries
      `AltaVdd 1.01` / `AltaVdds 1.0`, the regulator is presumably on the SCD
      SMBus, and the sibling board's notes already say prefdl gates this.
      **This is the M1 blocker and it blocks everything after it**
- [x] cold-vs-warm SCD diff taken 2026-09-23 — it ruled the reset block **out**:
      warm reads `0x000`, the exact state we produced by hand. Note an SMBus
      write to a regulator would leave no trace in a register diff at all
- [x] **the four devices are mapped** — `saguaro` is the SCD; `prickle` (v42)
      and `quill` (v85) are read through SCD `0x160`/`0x170`; **`thorn` (v34) is
      an Altera EPM240 CPLD on the host southbridge SMBus at `/sb/1` addr
      `0x23`**. See
      [hardware.md](hardware.md#the-four-devices-and-where-each-one-lives)
- [x] **`thorn` read cold versus warm, 2026-09-24** — and exactly one register
      differs: **reg 5 is `0x01` cold and `0xa1` warm**, bits 7 and 5. Same
      static binary both sides. See
      [hardware.md](hardware.md#thorn-register-5-cold-versus-warm)
- [x] the whole 256-register file read warm: only regs 1, 5 and 6 are non-zero,
      so reg 5 is the only candidate and there is nothing better hiding
- [x] `spike/thorn-read.c` has a guarded write path — `-w` requires `-b`, says
      what it will write, and reads back
- [x] **the thorn write is no longer the plan.** The vendor's board module
      shows `Thorn`'s methods are `isPowerPhaseFault`, `clearPowerPhaseFault`
      and `clockSelectStatus` — reg 5's bits are **status**, and writing them
      would almost certainly have done nothing
- [x] **found where the power devices are**: not on the host SMBus at all, but
      behind the SCD's accelerators — **CHL8228G (`ir`) at `/scd/1/1/0x70`** and
      **UCD90160 (`dpm`) at `/scd/0/1/0x4e`**, exactly where the board module
      said. The regulator's first 48 registers are captured warm in
      [hardware.md](hardware.md#where-the-two-power-devices-actually-sit)
- [ ] **SCD SMBus access for this board in `internal/platformhal`**, using the
      existing GPL-2.0 `scdsmbus` package. ⚠ Do **not** reimplement the
      accelerator protocol in a C spike: that directory is Apache-2.0 and a
      hand-written port is the same derivative work in another language
- [x] **CHL8228G read cold and diffed: all 48 registers identical to warm.**
      The regulator is configured on a cold board already, so programming it is
      not the missing step and the rails hypothesis is disproven. See
      [hardware.md](hardware.md#the-regulator-is-identical-cold-and-warm--the-rails-hypothesis-is-wrong)
- [ ] read the **UCD90160** (`/scd/0/1/0x4e`) cold — not yet done, the thermal
      restart storm ate the output
- [x] **the reset bits are named and we had them right**: `alta`=1, `sol`=2,
      `rpt`=8, and a cold `0x4000` reads `0x106` — exactly those three. The
      reset release was correct; the gap is upstream of it
- [x] **the board's whole SCD register map is recovered** — see
      [hardware.md](hardware.md#the-scd-register-map-for-this-board). It names
      the `0x3400`, `0x3800` and `0x5000` deltas from the earlier diff, and
      gives `portStatusControl[1..52]` at `0x5010` stride `0x10`
- [x] **the five steps before the reset are not it**, and `DosBoard` is the
      *diagnostic* tree rather than the boot path. `Saguaro.initialize()` is
      accelerator setup; `Ucd90160.initialize()` clears faults
- [x] **the `0x70` device is identified: it is the Si5338**, confirmed by its
      own `i2cAddress` register at `0x1b` reading `0x70`. So the "regulator
      identical cold and warm" result was the *clock*, and the CHL8228G has
      never been read
- [x] **Si5338 `0x30`–`0x70` read cold and warm: all 65 identical.** The
      multisynth programming is the same on a cold board, so the clock comes up
      configured
- [x] **Si5338 `0xda`–`0xf6` and `0xff` read cold: identical to warm**,
      `outputDrive` at `0xe6` included (`0x00` both). **The clock hypothesis is
      closed** — every register on it is the same cold as warm
- [x] **found the CHL8228G at `/scd/0/3/0x70`** — the board class carries the
      whole SMBus map as `accel.bus` (`ir 0.3`, `dpm 0.5`, `osc 1.1`, …), and
      `osc 1.1` matches where the Si5338 was found, which confirms the map.
      Earlier scans missed it because they probed register 0, and on a PMBus
      part command `0x00` is `PAGE` and does not answer a read
- [x] **the rails are already on cold.** `OPERATION` = `0x88` (enable bit set)
      cold *and* warm, and `READ_IOUT` is non-zero cold — the regulator is
      delivering current before anything NOSaic does. **The power hypothesis is
      closed.**
- [x] **the root port sees no device and no link cold.** `LnkSta = 0x1100`
      (DLActive clear, speed 0) and `SltSta = 0x0000` (PresDet clear) against
      `5GT/s x4 DLActive+ PresDet+` warm. The host side is fine; the FM6000 is
      not driving its PCIe receivers. See
      [hardware.md](hardware.md#the-root-port-sees-nothing-no-link-no-presence)
- [x] **the chip's PCIe comes up from an SPI boot ROM, and only from there.**
      Datasheet §7.2: the PCIe pairs are held inactive until initialised "via
      an external ROM". `PIN_STRAP = 0x208` with `bootMode[2:0]` at bits 9..7
      (per the vendor's register header, which also confirms `PIN_STRAP_STAT`
      is `MGMT2+0x021` = `0x1c021`) gives **`BOOT_MODE = 0x4` — boot from SPI
      serial ROM**. The host cannot substitute for this: it would need PCIe to
      do it
- [x] **the link stays dead after a real release** — `asic-release` runs
      through our own SCD driver every boot, and reading the root port after it
      still gives `LnkSta = 0x1100`, DLActive clear. Measured, not inferred
- [x] **the SCD's SPI block is not an arbiter.** `0x7900` is a plain master
      FIFO — `spicmd`/`spiread`/`spictrl`, no owner or mux field — and the
      FM6000 has its own SPI pins (`GPIO[3..6]`), so its boot flash is its own
- [x] **`thorn.initialize()` writes nothing to hardware** — it sets a software
      `operatingMode` field. Dead end, recorded so it is not walked twice
- [x] **the EOS timeline is measured**: scd loads at t=34, `scd_finish_init` at
      t=156 (interrupt/UIO plumbing, no reset write — the GPL source is
      readable), and the FM6000 appears at t=173 **by rescan**. So the release
      is done by **EOS userspace after the SCD driver is fully initialised** —
      not by the driver and not by `NorCalInit`
- [x] **it is a reset PULSE, not a release.** `dmesg` shows `pcielw` logging a
      link **down** and then a link **up** 104 ms apart at t=173, immediately
      before the chip is enumerated. On a port with nothing attached there is
      no link to lose, so that is the endpoint being taken down and brought
      back. This port has only ever cleared the reset bits; the vendor's
      diagnostics assert *then* clear, which was read as ceremony
- [x] **the pulse is ruled out.** With the bits corrected to `[1, 2, 8]` the
      driver asserts all three and releases them in order; `0xe1004000` then
      reads `0x00000000` on a cold board and the chip still does not appear.
      No link event is logged at all, where EOS logs a down and an up
- [ ] **instrument EOS's own boot.** Our sequence produces no link transition;
      EOS produces one at t=173. `spike/scd-dump.c` is already on the switch's
      flash, so a script started early under EOS that polls `0x4000` and the
      root port's `LnkSta` into a file on flash would catch the transition and
      what precedes it. That is the one event nothing so far has been able to
      see, and guessing at it has now cost four hypotheses
- [x] `CONFIG_HOTPLUG_PCI_PCIE` enabled, so a chip whose link comes up is
      enumerated without a manual rescan — which is what `pcielw` does for the
      vendor
- [ ] **so either the straps never latched, or `CHIP_RESET_N` is not the SCD's
      `alta` bit.** Those are the two remaining explanations and they are
      distinguishable: find what drives `CHIP_RESET_N` on this board — `thorn`
      is the obvious suspect, being the power sequencer with a
      `clockSelectStatus` already
- [ ] **find out what owns the SPI flash.** The SCD has its own SPI block at
      BAR0 `0x7900`. If the boot flash is shared between it and the FM6000 —
      the ordinary way to build this, so the host can reprogram it — then
      something arbitrates, and on a cold board the default owner may not be
      the chip. That would explain every measurement so far
- [ ] **check whether `CHIP_RESET_N` is the SCD's `alta` bit at all.** The
      straps are latched when it de-asserts, and a chip whose straps were never
      latched has no boot mode
- [ ] **the question is now the chip's own boot sequence, not the board's.**
      `SOFT_RESET` holds the FM6000's PCIe block at reset by default, and
      Table 4-1 step 11 says the BOOT ROM sets up the PCIe SerDes and releases
      it — inside the chip, before a host can help. Look at: the `BOOT_MODE`
      straps (`PIN_STRAP` read `0x208` warm), the serial EEPROM the boot
      controller reads and whether this board has one, and what `CHIP_RESET_N`
      is actually wired to versus the SCD's `alta` bit
- [ ] **the PCIe link itself is the remaining place to look.** Powered, clocked
      and out of reset, and still not on the bus, means the link is not
      training. Look at the RS780 root port `00:04.0` — its link status cold
      versus warm — and at datasheet Table 4-1 step 11, *"If PCIe is used, BOOT
      ROM must setup PCIe SerDes and take PCIe out of reset"*, which is a step
      inside the chip that nothing on this board has been shown to perform
- [ ] **step back.** Everything compared so far — reset block, clock, thorn —
      is identical cold and warm. "A device needs programming" predicts a
      difference and there is none. Next candidates, in order: find the
      CHL8228G (still unmeasured); look for something a register diff cannot
      see (a pulse, a GPIO, a strap); and examine the **RS780 root port at
      `00:04.0`**, which has never been looked at and which a chip needs in
      order to train even when it is powered, clocked and released
- [ ] **find the CHL8228G.** It is not at `0x70` on any accelerator/bus scanned
      so far, and its other address `0x30` answered nowhere either
- [ ] **the Si5338 clock generator.** The production path (`NorCalInit`, which
      is what `/etc/rc.d/init.d/NorCal` runs) programs a Silicon Labs Si5338 —
      "Programming clock for sid", with a `/mnt/flash/skipClockProgram` escape.
      An ASIC with no reference clock will not train a PCIe link whatever its
      resets say, which is this board's exact symptom. **Strongest remaining
      candidate**
- [ ] **find what actually releases the ASIC reset under EOS** — it is not in
      `NorCalInit` at all. Likely the vendor's `scd` kernel driver on probe, or
      the platform agent. Separate question from what makes the chip ready
- [ ] **`thermal` restart-storms** now that `platform_hal` is declared, because
      the board has no `smbus:` sensor map. Same fix as `nosd`: back off rather
      than spin. Do **not** fix it by inventing a sensor map A 240-element MAX II CPLD on the CPU's
      own i2c bus is the part and the placement of a power sequencer, and the
      best candidate for what holds the Alta unpowered. Reachable with
      `i2c-piix4` on the SB700 — no vendor path needed. Do it from our own image
      or Aboot: under a running EOS the read returns "Smbus transaction failed"
- [ ] SCD support for *this* board in `internal/platformhal/scd`, once the above
      is known: same FPGA family as the sibling Arista boards, different layout
- [ ] then release the FM6000 and have `02:00.0` enumerate
- [x] map BAR0 (`0xe2000000`, 32 MB) and read something that identifies the
      chip — `fm6000-probe` does, over sysfs `resource0`, needing no
      `iomem=relaxed`
- [ ] **find the addresses `boot.c` is missing**: `PLL_STATUS`, `SOFT_RESET`,
      `BOOT_STATUS`, and `BOOT_CTRL`'s field layout. The cold-boot sequence
      stops at step 6 naming exactly these, and each one deleted from that list
      is a step that starts working
- [ ] **set the Alta core rails from prefdl** — `AltaVdd 1.01`, `AltaVdds 1.0`
      on this board, per-board data rather than a constant
- [ ] find out what the SCD's second BAR (16 MB at `0xe0000000`) is for

## Shipping: what a cold board does with no operator

**As of 2026-09-25 the image brings the ASIC up by itself.** Verified by
booting it and touching nothing:

```
nosd-fm6000: looking for the chip, up to 30s
nosd-fm6000: reaching the chip over the SCD's local bus at 0000:04:00.0
nosd-fm6000: Intel FM6000 via SCD 0000:04:00.0 BAR1, 16 MB window
nosd-fm6000: cold chip -- running the documented boot sequence
   1   ok   chip out of reset and on the bus
   ...
   8   ok   BOOT 3: initialize all scheduler freelists
```

Three things had to change for that, and each was a way the old arrangement
could not have shipped:

- **the daemon looked for the chip on PCIe only.** It waited 30 s for an
  `8086:155b` that does not appear until the chip is configured, then exited
  non-zero. `fm_open_auto()` tries PCIe and falls back to the SCD's local bus.
- **the cold boot was opt-in.** Nothing else runs Table 4-1 — the platform HAL
  releases the reset and stops, because what to do with a released chip is the
  datapath's business — so the image booted to a prompt with a dead ASIC and
  nothing saying why. It now runs by default; `--no-boot` holds a chip cold.
- **it had to become safe to repeat.** `restart: always` means a restart would
  have put a forwarding chip back through the sequence.
  `fm_boot_already_done()` tests the end state rather than remembering
  anything, so it is right after a crash and right for a chip somebody else
  booted.

⚠ **The image is netboot-only in practice.** Nothing has been installed to this
unit's flash, because it is the lab's OSPF router and its EOS is the only way
back. `boot_mib`/`slot_mib`/`data_mib` are now measured, so the install path is
described; it has not been exercised.

What the switch honestly reports once up:

```
driver fm6000   ports 0 max   vlans false   l3 false   acl no   ecmp no
```

That is accurate and it is the gap: the chip boots, and nothing forwards yet.

## Platform parity with the other boards — mostly done

Features that do not need the ASIC, measured and working 2026-09-25:

- [x] **transceivers** — all 52 cages, presence and full SFF-8472 DOM.
      `nosaic platform transceivers` and `platform xcvr N`.
- [x] **temperature** — `board` 29 °C and `remote` 26 °C through
      `platform status`.
- [x] **PSU presence** — both supplies, free from the SCD driver.
- [x] **resets and watchdog** — released state reported; watchdog arms and
      recovers the box.
- [ ] **fans** — the controller is not on `0x58`–`0x68` of accelerators 0 or 1.
      Until it is found no cooling loop runs, which is now enforced by
      `wantsThermalService()` rather than left to chance.
- [ ] **board identity / prefdl** — `platform status` still says the SEEPROM is
      not located. Two non-transceiver `0x50` responders were found at accel 0
      bus 4 and accel 1 bus 0; read them. This also removes the hand-written
      MAC in `config/network.conf`.
- [ ] **status lamps** — the SCD LED registers are mapped
      (`0x6050`/`0x6060`/`0x6090`, `portLinkLed` at `0x60d0` stride `0x10`) but
      no `statusleds.conf` is generated for this board.

## M2 — the chip survives being talked to

This is where the board is actually hard. An access to an uninitialised bank
memory raises an uncorrectable ECC error, the chip escalates it to fatal, and the
endpoint leaves the PCIe bus — everything reads `0xffffffff` and the host sees a
hang rather than an error.

- [x] **a way to detect off-bus immediately and say so** — done:
      `datapath/fm6000/pci.c` confirms an all-ones read against PCI config
      space, latches, and refuses everything afterwards; `nosd-fm6000` reports
      the transition once, loudly; `fm6000-probe` names the exact word that did
      it
- [ ] the cold MGMT dump, diffed against the warm fingerprint in
      [hardware.md](hardware.md) — needs the chip on the bus first, so it is
      blocked behind M1
- [ ] **run the documented boot sequence in the documented order** (331496-002
      Table 4-1, reproduced in [hardware.md](hardware.md#the-documented-boot-sequence)):
      scan chain → PLL → modules out of reset → FFU slice numbers → **bank
      memory repair** → freelists. Steps 8–10 are `BOOT_CTRL:Command` writes
      polled on `BOOT_STATUS:CommandDone`
- [ ] does that alone make the bank memories safe to touch? This is the
      experiment that decides whether M2 is a day or a month, and it has not
      been run in this order
- [ ] only if it does not: work out what the documented minimum leaves out. The
      prior investigation says a per-block scan program is needed and the
      datasheet says one write is. Find out which, and write down why

## M3 — a documented cold init

The deliverable is not a sequence that works. It is a sequence where every write
has a reason, in code, parameterised by the board.

- [x] **the documented cold boot runs end to end, 2026-09-25** — Table 4-1
      steps 1-10, every step `ok`, verified on hardware. `BOOT_CTRL` ends at
      `0x313`, bit-for-bit what a forwarding EOS chip reads, and `SOFT_RESET`
      at `0x00`. The three addresses that were gaps are settled: `SOFT_RESET`
      word `0x9` (`0x1f` cold = all five modules held), `PLL_STATUS` `0x1c046`,
      and `BOOT_STATUS` — which is `BOOT_CTRL`, there is no second register.
      MSB is released *after* the boot commands, not with the others.
- [x] **running it fixes the EPL read hazard** — the experiment `hardware.md`
      said nobody had run. Before: reading `0x0e3b02` takes the chip off the
      bus. After: `0x00080000`, chip still answering. `fm_hazard()` now refuses
      EPL only until `fm_boot_mark_done()`.
- [x] **step 12, memory initialisation — done 2026-09-25.** The software fill,
      not the CRM. Before it, reading `0x200000` takes the chip off the bus;
      after it the read is safe. Table 4-1 now completes but for step 11.
- [x] **the bank model was wrong and is now measured.** One memory
      (`0x200000`–`0x23ffff`, twice the modelled span), not three. `0x240000`
      and `0x260000` are register blocks whose fills die at `0x240036` and
      `0x260014` — both bisected exactly.
- [ ] **what the STATS region actually holds.** The fill makes it readable but
      it does not read back what was written, and writing `0` and `0xa5a5a5a5`
      give the *same* readback, so the value is not derived from the write.
      Stable across reads, so not a counter. Needs its own look.
- [x] **the SPICO/licensing risk is settled, and favourably.** SerDes firmware
      is needed only for **copper**; a fibre-only build needs zero proprietary
      files. This board is 52 × SFP+ with fibre SR modules in it, so the
      redistributable claim holds. Copper DAC is out of scope until somebody
      reimplements SPICO. This was the open risk that could have invalidated
      the licensing shape of the whole port.
- [x] **the EPL block is mapped** — 24 EPLs × 4 lanes at stride `0x80` from
      `0x0e0400`, two structures interleaved, with `EPL_CFG_A`/`B` landing
      where independently known. ⚠ Above `0x0e6400` it is still fatal to read.
- [x] **the SBus is documented, and Table 9-4 maps EPL to SBus address.** §9.4
      gives the command sequence outright. 96 Ethernet SerDes and 24 EPLs,
      matching the block sweep exactly. ⚠ Table 9-4 prints `EPL[6]` twice and
      omits `EPL[7]`; SBus 17 is EPL[7].
- [x] **the SBUS_COMMAND field layout is confirmed on hardware** — register
      `[7:0]`, device `[15:8]`, op `[23:16]`, EXECUTE bit 24, bit 25 behaves
      as BUSY, bit 28 appears after a transaction.
- [x] **the SBus works, 2026-09-26.** All 24 EPL lane-0 SerDes answer rc=4 and
      a device that cannot exist returns rc=6. `SBUS_CFG` bit 0 is a **reset**,
      not a clock ratio — clearing it is what makes the bus run. rc 4 = ok,
      rc 6 = no device. ⚠ Register 0 reads zero on every device, so never
      judge a transaction by its data; `fm_sbus_present()` uses register 2 and
      the result code.
- [ ] **bring a port up.** The two gates are known: `EPL_CFG_B.PortNPcsSel` = 3
      for 10GBASE-R (ours reads `PCS_DISABLE` today) and `EPL_CFG_A.Active_N`.
      Both are per-EPL registers with per-port fields. Needs the lane-enable
      algorithm — 18 steps, read-modify-write and two blocking polls — and an
      SBus op-`0x20` device reset before any SerDes write.
- [ ] **step 11, PCIe** — `fmPlatformSetupPCIe` is six registers we have
      addresses for (`0x01002`, `0x01400`, `0x01416`, `0x01418`, `0x0141d`,
      `0x01435`) and have never tried. Needed for packet DMA, not before.
- [ ] **the 17 MGMT words our boot does not set** — SWEEPER `0x1c048`, the
      interrupt masks at `0x1c001`/`0x1c002`, `0x1c01e`, `0x1c049`/`4b`/`4c`/
      `50`. These are configuration rather than boot, so they belong here only
      as a list of what M4 has to account for.
- [ ] SBus master and SerDes SPICO up
- [ ] **find the parser Action SRAM** in the register map: where it is, how many
      slices this part has, how a slice's SRAM is indexed by state. The
      encoding is documented (331496-002 Table 5-3); the address is not
- [ ] **measure how much works with an empty parser** — does a port link, does
      a frame reach the CPU, what exactly stops. Sizes everything below it
- [ ] a parser microcode **generator**: takes what we want parsed, emits the
      per-slice Action SRAM words. Ours, Apache-2.0, in this tree. First target
      is the smallest thing that can be checked — Ethernet and nothing else
- [ ] extend it to VLAN/QinQ, IPv4, IPv6, TCP/UDP, ARP, checking `FIELDS`
      against Table 5-5 and `ACTION_FLAGS` against §5.5.9 at each step
- [ ] the FFU and mapper are **tables, not microcode** — generated from the port
      map and rule set like any other board's forwarding state
- [ ] block-by-block: MGMT, CM, PARSER, MAPPER, POLICERS, L2AR, SCHED, MOD, EPL.
      Each one gets a section in `hardware.md` saying what it does and what our
      code writes into it, or it is not done

## M4 — a port comes up

- [ ] the port map: front panel 1..52 → EPL instance → serdes lane, measured on
      the bench one cage at a time. Nothing else can be right before this is
- [ ] `EPL_CFG_B` PCS select set to 10GBASE-R and one cage links to a known-good
      far end
- [ ] SFP laser enable through the SCD, and cage presence/EEPROM
- [ ] **settle the SPICO question early** — is SerDes microcontroller code a
      separate vendor firmware file, embedded in the proprietary SDK, or not
      needed on this part? It decides whether images for this board can be
      published, so it invalidates licensing decisions if left late

## M5 — packets reach the CPU

- [ ] packet DMA ring at BAR0+`0x5000`, TX and RX, without an IOMMU. **This is
      implementation from a specification**: §7.11 documents the engine and
      Table 7-5 the 16-byte descriptor (Status / Length / Buffer-Addr-Lo /
      Buffer-Addr-Hi), power-of-two rings, 32-byte aligned
- [ ] the CPU port's frame format. Table 7-8 gives the F64/ISL tag as **7 bytes
      at L2 offset 12** (DGLORT/SGLORT/SWPRI/USER/FTYPE); the prior work on this
      chassis measured **8** there. Settle which on the bench — the tag goes
      inline in the frame, not in the descriptor field that looks like it
- [ ] a tap per front-panel port, bridged the way `datapath/common/tapbridge.c`
      does it for every other board

## M6 — it forwards

- [ ] L2 in hardware between two front-panel ports
- [ ] L3, and the FIB mirror that every other datapath already shares
- [ ] `nosd-fm6000` answering the same switch-api socket as every other provider,
      so nothing above it knows the chip changed

## M7 — the board, not the chip

- [ ] **arm the watchdog**, and check this board's bit assignments for it.
      `0x0120`, bit 31 enables, `[30:29]` = 2 is a power cycle. Aboot hands over
      with it disarmed. On a board that cannot hardware-reset this is the only
      recovery that is not a human at the PDU, so it gates whether A/B rollback
      means anything here — see M0's reboot question
- [ ] LEDs, including the chassis status lamps
- [ ] sensors and a cooling curve measured on this chassis
- [ ] transceiver presence and EEPROM for 52 cages
- [ ] A/B install and rollback — gated on M0's reboot question

## Worth taking from elsewhere rather than writing

- [x] **`aristanetworks/sonic`** — checked 2026-09-23, and the useful half of
      what was hoped for is not there. **There is no `raven` support in it**:
      the platform list runs `clearlake`, `upperlake`, `lodoga`, `blackhawk`
      and newer, and the tree contains no `raven`, `norcal`, `fm6000` or `7150`
      at all. This board's platform layer has to be written, not borrowed.

      What it *did* confirm is the reset convention, from `src/scd-reset.h` and
      `src/scd-reset.c`: `RESET_SET_OFFSET 0x00`, `RESET_CLEAR_OFFSET 0x10`,
      and a release is `write (1 << bit)` to the clear offset. That is exactly
      what worked on this board, now corroborated by the vendor's own driver
      rather than inferred from the sibling port. `src/scd.c` is a generic FPGA
      driver — i2c/SMBus master, GPIO, LEDs, transceivers — with no board power
      sequencing in it, so it does not answer the M1 question
- [ ] **`aristanetworks/swi-tools`** is Arista's own SWI/SWIX packaging tooling.
      M0 should use it rather than hand-rolling the zip and `boot0`
- [ ] there is **no FM6000 SAI and the 7150 is not a SONiC platform** — Fulcrum
      was EOL around 2016. Nothing to borrow on the dataplane side, which is
      why this port writes one

## Not blocking, but write it down when it is learned

- [ ] what the `-CL-F` in the SKU changes, if anything
- [ ] whether the second unit (unit D) is identical; nothing has been read off it
- [ ] this board's `aboot_max_hwepoch` from its own prefdl
