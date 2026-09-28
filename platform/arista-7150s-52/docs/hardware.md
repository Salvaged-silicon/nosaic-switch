# Arista DCS-7150S-52 — hardware reference

The deep page: how this switch is built and how NOSaic will drive it. The
audience is somebody writing the FM6000 datapath or debugging silicon.

**Provenance matters more on this page than on any other board's.** NOSaic does
not run on this switch yet, so nothing below has been measured *by our code*.
Each table says where its numbers come from:

| Mark | Means |
|---|---|
| **live** | read off the running unit (`sw7150-lab`, EOS 4.16.8M) with `lspci`, `/proc/cmdline`, `prefdl` or a BAR0 probe |
| **documented** | in the Intel datasheet, cited by section or table |
| **derived** | worked out from the investigation and believed, but not confirmed from a second direction |
| **assumed** | stated so it can be checked; not evidence |

Every datasheet citation on this page is to **331496-002, revision 3.4** (352
pages). The only copy `make datasheets` can fetch is 331496-001 revision 3.3,
which is two pages longer and does not necessarily number things the same way —
see [docs/datasheets.md](../../../docs/datasheets.md).

## ⚠ Read this first: the chip is not reached over PCIe

**Most of the investigation below asks the wrong question.** It asks what puts
the FM6000 on the PCI bus, on the assumption that the bus is how you reach its
registers. It is not, and on a cold board it cannot be.

The SCD is a **PCI-to-LocalBus bridge**, and the FM6000's entire register space
is mapped into **the SCD's BAR1** — 16 MB at `0xe0000000`, word-addressed, base
zero. Register word `w` is at byte `4*w`. That window is live as soon as the SCD
enumerates, which it does unconditionally at power-on, with no help from
anybody.

So there is no chicken-and-egg. The FM6000 never had to be on the PCI bus for us
to talk to it. PCIe on this part is for **packet DMA**, and it trains *late*, as
a consequence of the chip having been configured over the local bus.

### How this was found

The vendor says so in its own log. `/var/log/agents/FocalPointV2-3350`, the
FM6000 datapath agent, opens with:

```
Operational mode: modeNormal
Returning localBusHam for alta0          <-- local bus, not PCIe
Ring mode set to 5 (52 ports, 64 tokens, 4 locked, 4 slow, 1 sync)
Using microcode init func fm6000UcLibraryInit
SwitchNum = 0
SERDES lanes are ready in 13836 usec. PollCnt 109. PCI_IP = 0x20
Chip version is B2 ( 64 ports )
```

and closes, 180 lines later and after the whole port map has been programmed,
with:

```
Returning pciHam for alta0               <-- only now
```

A "ham" is Arista's hardware access method. The agent uses the local bus for
bring-up and switches to PCIe at the end.

Two files on the box are **plain Python source, not compiled**, and spell out
the mechanism:

- `/usr/lib/python2.7/site-packages/FruPlugin/FocalPointV2.py:211`
  ```python
  localBus = fru.localBus.otherEndConnectedTo
  assert localBus, "Device is not connected to a PCI-to-LocalBus bridge"
  ```
- `/usr/lib/python2.7/site-packages/FruPlugin/Scd.py:204`
  ```python
  for name in scd.localBus:
     # scd includes a PCI-to-LocalBus bridge
     hamCtors = Fru.pciHotplugHam( scd.pciFpga, driverCtx, filename="resource1" )[1:]
  ```

`filename="resource1"` is the whole answer: **SCD BAR1**.

### Confirmed against hardware, both ways

Four registers whose values were already known from PCIe reads on a forwarding
chip, re-read through SCD BAR1 on the same chip. **live**

| Register | word | byte in BAR1 | via SCD BAR1 | via FM6000 BAR0 |
|---|---|---|---|---|
| `PIN_STRAP` | `0x1c021` | `0x70084` | `0x00000208` | `0x208` ✓ |
| `BOOT_CTRL` | `0x1c022` | `0x70088` | `0x00000313` | `0x313` ✓ |
| `SWEEPER` | `0x1c048` | `0x70120` | `0x0008bb2c` | `0x0008bb2c` ✓ |
| `EPL_CFG_B` | `0x0e3b02` | `0x38ec08` | `0x00090003` | `0x00090003` ✓ |

16 MB is exactly 4M words × 4 bytes — the whole FM6000 register space, at
offset 0.

Then the same read on a **cold** board, NOSaic RAM-booted, `lspci` showing no
`8086:` device at all:

```
BAR0=0x00000000e1000000 BAR1=0x00000000e0000000
  PIN_STRAP  w=0x1c021  0x00000208
  BOOT_CTRL  w=0x1c022  0x00000320     (0x313 warm -- unconfigured, as expected)
  SWEEPER    w=0x1c048  0x00000000     (unconfigured)
```

`PIN_STRAP` is a hardware-latched strap: it does not depend on configuration, so
a cold chip and a forwarding chip *should* agree, and they do. Reading `0x208`
rather than `0x00000000` or `0xffffffff` is the chip answering.

### It takes a reset PULSE, not a release

This is the part that made the earlier experiments look like failures. **live,
2026-09-25**

| state | `PIN_STRAP` reads |
|---|---|
| resets left clear since boot (`resetSet` = `0x0`) | `0x00000000` — silent |
| after asserting `0x106` to `resetSet` | `0x00000000` — silent |
| **after clearing `0x106` to `resetClear`** | **`0x00000208` — answering** |

So the chip needs the reset **driven and then let go**, not merely found
released. NOSaic's own `release-asic.sh` leaves the bits clear at boot, which
reads identically to "never asserted" and produces a silent chip. Once pulsed
the chip is stable indefinitely: 400 consecutive reads and a 5 s idle both
returned `0x208`.

This also makes bring-up experiments cheap. A pulse revives the chip in under a
second, from the shell, with no power cycle — so killing it costs nothing.

### ⚠ Reading the EPL block on an unconfigured chip kills it

Measured by bisection, reading `PIN_STRAP` between each step: **live**

| read | chip afterwards |
|---|---|
| `BOOT_CTRL` `0x1c022` | alive |
| `SWEEPER` `0x1c048` | alive |
| `SCAN_CFG` `0x1c03a` | alive |
| **`EPL_CFG_B` `0x0e3b02`** | **dead — `PIN_STRAP` reads `0x00000000`** |
| anything, after that | dead, until the next pulse |

This is the ECC-uninitialised hazard the datapath already guarded in the
abstract, now measured and localised to the EPL block. `fm_hazard()` refuses all
of `0x0e3000`–`0x0e4fff` until the boot sequence has run.

Note the failure *signature* differs by transport. A dead PCIe endpoint answers
`0xffffffff`; a chip knocked off the **local bus** answers `0x00000000`, because
there is no PCIe error semantics in the path to produce the all-ones. Code that
tests for `0xffffffff` will not notice. `fm_alive()` tests `PIN_STRAP` instead.

### What this changes

- **M1 is not a blocker and never was a real one.** Register access has been
  available from cold the entire time.
- The four hypotheses below — reset bits, the Si5338 clock, the CHL8228G rails,
  the reset pulse — were all tested against the wrong success criterion
  ("does it appear in `lspci`"). The right one is "does `PIN_STRAP` read `0x208`
  over BAR1", and the answer is yes, after a pulse.
- The next work is the **Table 4-1 boot sequence over the local bus**, not
  further hunting for what enumerates the chip.
- Everything below this section is kept as written. It is a record of how the
  question was got wrong, and several of its measurements (the SCD register map,
  the reset bit names, the SMBus map, the device identifications) remain correct
  and useful. Where a section's *conclusion* is superseded, it is superseded by
  this one.

## The documented cold boot runs, 2026-09-25

**Table 4-1 executes end to end on this board and leaves the chip in the same
boot state as a forwarding EOS chip.** No replay, no vendor blob — the public
datasheet's twelve steps, three register addresses measured here, and the
board's own reset pulse.

```
   1   ok   chip out of reset and on the bus
   2   ok   boot from CPU selected
   3   ok   core logic and EPLs to normal operating mode
   4   ok   PLL locked
   5   ok   EPL, PCIe, MSB, SPICO/SBUS out of soft reset
            released, MSB last after the boot commands
   6   ok   BOOT 1: initialize FFU slice numbers
   7   ok   BOOT 2: apply bank memory repairs
   8   ok   BOOT 3: initialize all scheduler freelists
   9  skip  PCIe serdes up and out of reset
  10  skip  memory initialised (CRM, or software fill)
```

### The three addresses that were missing

All three were gaps that made the sequence refuse rather than guess. **live**

| register | word | how it was settled |
|---|---|---|
| `SOFT_RESET` | `0x000009` | reads `0x1f` cold — all five module bits — and `0x00` on a forwarding EOS chip |
| `PLL_STATUS` | `0x01c046` | `0x3` cold, `0x7` on EOS, `0x0f` after our boot; `[1:0]` PLLs, `[3:2]` DLLs |
| `BOOT_STATUS` | `0x01c022` | **it is `BOOT_CTRL`** — there is no separate register |

`SOFT_RESET` is at word 9, almost at the bottom of the address space, which is
why sweeping `0x1a000`, `0x1b000`, `0x1c000`, `0x1d000` and `0x1e000` for it
found nothing. Its bits are PCIe 0, MSB 1, FIBM 2, JSS 3, EPL 4, and a set bit
means *held*. A cold chip reading `0x1f` is precisely the datasheet's "default
value is to assert reset on all modules".

`BOOT_CTRL`'s two readings decode cleanly under one layout, which is what makes
it believable rather than merely asserted:

```
   cold  0x320  =  EepromLoadDone (bit 5),  Command 0
   warm  0x313  =  CommandDone    (bit 4),  Command 3   <- the LAST of the
                                                            three Table 4-1
                                                            commands
```

⚠ **MSB comes out of reset LAST.** Releasing the core fabric before the boot
controller's bank-repair and freelist commands have run drops the CPU into an
unconfigured fabric and hangs it. So step 7 is split: everything except MSB
before the commands, MSB after.

### The state it reaches, against EOS

MGMT block, 4096 words, three ways. **live**

| word | cold | after our boot | EOS | |
|---|---|---|---|---|
| `0x1c022` | `00000320` | `00000313` | `00000313` | ✅ `BOOT_CTRL` |
| `0x1c03a` | `00000000` | `ffffffff` | `ffffffff` | ✅ scan config |
| `0x1c03b` | `00000000` | `ffffffff` | `ffffffff` | ✅ scan chain |
| `0x1c046` | `00000003` | `0000000f` | `00000007` | PLLs+DLLs; ours locks one more than EOS |
| `0x1c045` | `00000000` | `00000003` | `00000000` | we write DLL enable; EOS does not read back |
| `0x1c048` | `00000000` | `00000000` | `0008bb2c` | SWEEPER — configuration, not boot |
| `0x1c01e` | `00000000` | `00000000` | `fffc0000` | configuration |
| `0x1c049`/`4b`/`4c`/`50` | `0` | `0` | set | configuration |

Our boot matches EOS on exactly the words Table 4-1 specifies. The seventeen
that still differ are **post-boot configuration** — the sweeper, interrupt
masks, per-block setup — which is M4 work and not part of the documented cold
boot. That is the expected shape of the result, not a shortfall.

### ✅ Running Table 4-1 fixes the EPL read hazard

The experiment this page said nobody had run. **live**

```
   before the sequence:  read 0x0e3b02  ->  chip off the bus, PIN_STRAP = 0
   after  the sequence:  read 0x0e3b02  ->  0x00080000, PIN_STRAP = 0x208
```

So the hazard is not permanent. ⚠ **But it is not wholly cleared either, and
the first version of this section over-claimed from a single word.** Sweeping
the block after the boot sequence shows:

```
   0x0e0400 .. 0x0e6316   reads fine, and has real structure in it
   0x0e6400 .. 0x0e7fff   STILL takes the chip off the bus
```

`fm_hazard()` refuses the block until `fm_boot_mark_done()`, which
`fm_boot_cold()` calls itself on success — and that remains the right guard,
because the upper part is fatal in both states.

This does **not** unlock the ECC bank memories. Those are a separate guard
waiting on step 12's memory initialisation, which is not written yet.

### ⚠ A second read hazard: `0x1a000`

Reading the 4096 words at `0x1a000` takes a pre-boot chip off the bus, the same
way EPL does. Found while sweeping for `SOFT_RESET`. `0x1b000`, `0x1d000` and
`0x1e000` are all safe. Not yet identified, and not yet guarded, because
nothing needs to read it. **live**

## Platform features work: optics, sensors, PSUs, 2026-09-25

None of this needs the ASIC, and all of it is verified on hardware. **live**

```
cage  type  state                     raw
1..4  SFP+  module present, laser on  0x00000180
5..52 SFP+  empty                     0x00000187

4 populated, 48 empty, 0 not powered, 0 undetermined, of 52 cages.

cage 1  CISCO-FINISAR FTLX8574D3BCL-CS  serial FNS215108H7
        29.7 C  3.31 V   rx -2.19 dBm  tx -1.95 dBm  bias 8.5 mA

temp board   29.0 °C        psu1  present
temp remote  26.0 °C        psu2  present
```

### The cage map, triangulated three ways

Not taken from a table — measured, and each way checks the others:

1. scanning address `0x50` across accelerators 0–9 and buses 0–7 found exactly
   **two** SFF-8472 identifiers (`0x03`), at **accel 2 bus 0** and **accel 2
   bus 1**;
2. the SCD's own per-cage registers read `0x1E0` at `0x5010` and `0x5020` and
   `0x1DF` at the other fifty — **the same two cages and no others**;
3. those two are `Ethernet1` and `Ethernet2`, the only ports EOS has up.

So **panel port N is SCD `0x5010 + (N-1)·0x10`, and SMBus accelerator
`2 + (N-1)/8` bus `(N-1)%8`.** Accelerators 2–8 give 56 buses for 52 cages, and
accelerators 10 and above answer "no response", so 0–9 is the set.

### The temperature sensor, and what it is not

One LM90-compatible part at **accel 0 bus 0 address `0x4c`**: local diode at
register `0x00`, remote at `0x01`.

It answers manufacturer ID (`0xfe`) `0x01` and device ID (`0xff`) `0x11`. That
is **not** Maxim, so it is not the `max6658` whose two registers are identical —
the board declares `lm90`/`lm90-remote`, named for the register layout rather
than for a part number nobody has confirmed.

⚠ What the **remote** diode is wired to is not established. It reads *cooler*
than the local one (26 °C against 29 °C), which argues against it being the
FM6000 die, so it is named `remote` rather than `asic`. **UNKNOWN**

### ⚠ No fan controller found, so no cooling loop runs

A scan of `0x58`–`0x68` on accelerators 0 and 1 found nothing. The chassis has
fans; where their controller sits is not known, and none is declared.

This board is the reason `wantsThermalService()` now requires **fans as well as
sensors**. A cooling loop with readings and nothing to drive exits "no fan
control" on its first pass, and `restart: always` respawns it for ever — the
same storm that twice destroyed console output here, reached from the other
side. A board with sensors and no fans still reports temperature through
`nosaic platform status`; it just does not get a regulator.

### A lead on the prefdl SEEPROM

The same `0x50` scan found two responders that are **not** transceivers,
answering identifier `0x01` rather than `0x03`: **accel 0 bus 4** and **accel 1
bus 0**. An SFF identifier of `0x01` is "GBIC", which no cage on this board
has, so these are more likely the board's own SEEPROMs. That matters because
`platform status` still reports *"board identity needs the prefdl SEEPROM,
which is not located yet"*, and because `config/network.conf` currently states
a MAC address by hand for want of one. Not yet read. **derived**

## Step 12 runs, and the bank model was wrong, 2026-09-25

Table 4-1 now completes. Only step 11, PCIe, is skipped, and deliberately.

### There is one bank memory, not three

This page used to say there were three — STATS, MCAST_MID, MCAST_POST — each
`0x20000` words. Filling them on hardware says otherwise. **live**

| address | what a fill does |
|---|---|
| `0x200000`–`0x23ffff` | **262144 words, filled in one go, chip fine.** A memory — and *twice* the modelled span; the old `0x20000` stopped halfway through it. |
| `0x240000` | **Not a memory.** 54 words in, writing **`0x240036`** takes the chip off the bus. |
| `0x260000` | **Not a memory.** 20 words in, writing **`0x260014`** does the same. |

Both fatal words were bisected exactly rather than bounded — binary search on
the fill count, with a reset pulse and a re-boot between trials, which costs
about ten seconds each now the box stays on NOSaic.

⚠ **And then re-bisected, because the first harness could not be trusted.** Two
runs of it disagreed about `0x260000`, reporting `0x260010` and `0x260014` from
the same script — they contradicted each other on the same trial, 20 words
"kills" against 20 words "ok". The cause is the one already described for the
EPL sweep: recovery was not verified, so a trial whose chip was *already* dead
reports "kills", and since a false "kills" moves the upper bound down, the
search is biased toward smaller indices.

The harness now **verifies recovery, retries it three times, and aborts** if it
cannot get the chip back, and it runs **three trials per point** and prints
`NOT REPRODUCIBLE` rather than averaging a disagreement away. Re-run under it,
both values come back unanimous at every point:

```
  0x240036    3/3 at every point
  0x260014    3/3 at every point
```

So the numbers above are right and the earlier disagreement was the harness,
not the chip. The lesson is the general one and it has now cost time twice on
this board: **a recover-and-retry harness that cannot distinguish "this killed
it" from "it was already dead" does not produce weak evidence, it produces
confident wrong answers** — and a bisect converges on one of them regardless.

So the two "MCAST" addresses are **register blocks** that happen to sit where
traffic to them was once observed, and treating a register block as fillable
memory is how you lose a chip. `fm_is_bank()` now covers the one real memory,
and `fm_hazard()` refuses those two words outright.

### What the fill achieves, and what it does not

**Achieves exactly what the step is for.** Before it, reading `0x200000` takes
the chip off the bus; after it, the read is safe and the chip keeps answering.
That is the whole purpose of "initialise memory", and it is now verified rather
than assumed.

**Does not tell us what the region contains.** It does not read back the
pattern written, and the readback is not a simple function of it: **UNKNOWN**

```
   wrote 0xa5a5a5a5  ->  reads 0x12180018 0x521a1800 0x0a080a48 ...
   wrote 0x00000000  ->  reads 0x12180018 0x521a1800 0x0a080a48 ...  (identical)
   wrote 0xffffffff  ->  reads 0x00000000 ...
   wrote 0x00000001  ->  reads 0x00000000 ...
```

Stable across repeated reads, so it is not a counter and not noise. Writing
zero and writing `0xa5a5a5a5` produce the *same* readback, so the value read is
not derived from the value written. Something else decides it. That is a
separate investigation and step 12 does not depend on the answer.

### ⚠ The guard cannot be answered by the chip here

`fm_boot_already_done()` works because the chip records its own boot state. There
is no equivalent question for "have the banks been filled", so a fresh process
starts with `banks_ready` clear and refuses the region — correctly. Reads of a
filled bank therefore belong in the process that filled it, which is why
`fm6000-probe --meminit` characterises the region itself instead of leaving it
to a second invocation. The alternative is a flag that turns the guard off, and
that is the one thing `pci.h` says not to build.

## The EPL block is mapped, and the SPICO question is answered, 2026-09-25

### ⚠ Zero proprietary files, for fibre

The page above called the SerDes firmware *"the open risk to the claim that
images for this board stay publishable"* and said settling it could invalidate
the licensing shape of the whole port. **It is settled, and the answer is the
good one.**

The prior EdgeNOS investigation on this same chassis got to *"zero proprietary
files for a fibre-only build"*. SPICO SerDes firmware is **not generatable —
only droppable, and only for fibre**; copper needs firmware nobody has
reimplemented. This board's 52 cages are SFP+, and the two modules in it are
CISCO-FINISAR FTLX8574D3BCL-CS, which are fibre SR. So the parser-microcode
decision and the SerDes question now point the same way and the redistributable
claim holds for the configuration this board is in. **Copper DAC is out of
scope until somebody reimplements SPICO.**

### The EPL block, measured

Swept after the boot sequence, in chunks, with recovery verified between them.
**live**

Two structures interleaved, at a stride of **`0x80` words** from `0x0e0400`:

| | count | what it is |
|---|---|---|
| **type A** | **96** | per-**lane** |
| **type B** | **24** | per-**EPL** |

24 EPLs × 4 lanes = 96 lanes, which is what the part should have, and type B's
offsets `+0x01` and `+0x02` land exactly on the independently known
`EPL_CFG_A` (`0x0e3b01`) and `EPL_CFG_B` (`0x0e3b02`). Two facts agreeing from
different directions is what makes this a map rather than a pattern.

```
 type A (per lane)                    type B (per EPL)
   +0x00  00000015                      +0x01  0c7d7899   EPL_CFG_A
   +0x01  0007ffff                      +0x02  00080000   EPL_CFG_B
   +0x02  07ffffff                      +0x03  00041041
   +0x03  00000080                      +0x13  00002985
   +0x04  00001003                      +0x14  000001ff
   +0x10  40000000                      +0x16  unique per EPL
   +0x13  00005381
   +0x16  0100009c                    ⚠ above 0x0e6400 the block is still
   +0x17  00000001                      fatal to read, booted or not
   +0x21  00001001
   +0x23  00000011
   +0x24  00000228
   +0x2a  02000000
   +0x2e  01048000
   +0x34  0aaaa005
   +0x37  00000001
   +0x3e  VARIES per lane: 0000aa80 x20, 00005560 x19, 0000ffe0 x14
   +0x3f  00000780
   +0x40  00003fff
```

`+0x3e` is the only per-lane field that differs, which makes it the first place
to look for lane state. **derived**

### The addressing, and it is completely regular

Every EPL owns **eight slots of `0x80` words — `0x400` words in all**:

```
   slot 0..3   the four lanes
   slot 4,5    empty
   slot 6      the per-EPL registers, EPL_CFG_A and EPL_CFG_B among them
   slot 7      empty

   per-lane:  0x0e0400 + (epl-1)*0x400 + lane*0x80
   per-EPL:   0x0e0700 + (epl-1)*0x400
```

The sweep found per-lane structures at instance indices 0-3, 8-11, 16-19 … and
per-EPL ones at 6, 14, 22 … 190. That is this layout and nothing else.

⚠ **The index is the FDL's `eplId`, not the datasheet's EPL number.** Those two
disagree, and it is the FDL's that the register block uses. EPL 14 — what the
FDL gives for front-panel port 1 — computes to `0x0e3b00`, and `EPL_CFG_A` and
`EPL_CFG_B` were independently found at `0x0e3b01` and `0x0e3b02`. The prior
investigation's `SERDES_IP` at `0x0e3841` also falls inside EPL 14 lane 0's
slot. Three agreements. **live**

Verified against hardware for EPLs 1, 13, 14, 15, 16 and 24: all read
`EPL_CFG_A 0x0C7D7899` and `EPL_CFG_B 0x00080000`, uniformly unconfigured.

### ✅ And it explains the "fatal region" above `0x0e6400`

EPL 24's block is at `0x0e6300`, so the last implemented word in the whole EPL
space is around `0x0e6316` — which is exactly where the sweep's last readable
data was. **Reading above `0x0e6400` is reading past the last EPL.** It is not
a hazard with a reason, it is unimplemented address space, and that is a much
more comfortable thing to have found than another mystery.

### Setting the PCS type, measured

Port 1 is EPL 14 lane 0. `EPL_CFG_B` reads `0x00080000` on a booted chip and
`0x00090003` on a forwarding one, so the PCS selector is `0` = `PCS_DISABLE`.

Writing the low nibble to 3 works and the chip stays up:

```
   before  CFG_B = 0x00080000
   after   CFG_B = 0x00080003
```

⚠ **And nothing else in that EPL moved** — not one word of either the per-EPL
slot or lane 0's slot. So the PCS selector alone is not what brings a port up;
the second gate, `EPL_CFG_A.Active`, still has to be set, and its bit position
is **not known** because `EPL_CFG_A` has never been read on a forwarding chip
here. That is the next measurement, and it needs EOS.

⚠ The selector also did **not clear** when `0x00080000` was written back over
it. Either the field is write-1-to-set or clearing it needs the port down
first. Not established. A reset pulse does clear it.



### The two gates that decide whether a port comes up

From the prior investigation's two-day hunt for one dark port, and worth having
before we try: both gates live in **per-EPL registers with per-port fields**, so
two lanes are *fields of the same register* — which is why every per-lane diff
it took reported "identical configuration", correctly and uselessly.

- `EPL_CFG_B.PortNPcsSel` — `3` is 10GBASE-R, `0` is `PCS_DISABLE`
- `EPL_CFG_A.Active_N` — the lane must be marked active

Ours currently reads `EPL_CFG_B` = `0x00080000` against a forwarding chip's
`0x00090003`, so the low nibble is `PCS_DISABLE` today.

⚠ Also from that work, and the reason a capture cannot substitute here: **a
lane enable is an algorithm**, 18 steps of read-modify-write plus two blocking
polls, and **an SBus write to a SerDes needs an op-`0x20` device reset first**.
A replay carries the values that came out, not the reads or the waits.

### ⚠ A harness that could not tell "killed" from "already dead"

Worth recording as method. An earlier sweep reported that *every* word in the
EPL block was fatal — a clean-looking table, and false. Its recovery helper
called the boot sequence and carried on without checking, and `--boot` bails at
step 1 on a chip that is not answering and does nothing. So one silent recovery
failure made every later verdict garbage, in the shape of "everything failed".

The sweep now verifies recovery and **aborts** rather than reporting. A harness
that cannot distinguish those two states produces confident nonsense.

## The SBus works, 2026-09-26

§9.4 documents the SerDes serial bus, which is the route to every lane.
**All 24 EPLs answer, and a device that cannot exist is correctly refused.**

### What the datasheet gives, and it is a lot

**96 Ethernet SerDes and 4 PCIe SerDes** on one slow ring, up to 6 µs per
access. 96 is exactly the number of per-lane structures the EPL sweep found,
and the 24 EPLs it implies is exactly the number of per-EPL ones — the
datasheet and the bench agree without being made to. **documented + live**

The command sequence is given outright (§9.4), which makes this one of the
few parts of this chip that needs no reverse engineering at all:

```c
    SBUS_REQUEST = data;                    /* writes only */
    SBUS_COMMAND = EXECUTE_BIT + (op << 16) + (device << 8) + reg;
    while (SBUS_COMMAND & BUSY_BIT) yield();
    data = SBUS_RESPONSE;                   /* reads only */
    SBUS_COMMAND = 0;
```

⚠ The datasheet prints `READ << 16` in **both** the read and the write
examples. The write one is a typo.

Only one master may have a command outstanding: the response register is
shared, and §9.4 spells out how two masters racing gives one of them the
other's answer. Nothing in the chip prevents it.

### Table 9-4, the EPL-to-SBus map

Each EPL owns four consecutive SBus addresses. The order is a physical ring
order and is **not** the EPL numbering, so it cannot be computed:

| SBus | EPL | | SBus | EPL | | SBus | EPL |
|---|---|---|---|---|---|---|---|
| 1 | PCIe | | 33 | EPL[19] | | 65 | EPL[22] |
| 5 | EPL[1] | | 37 | EPL[20] | | 69 | EPL[23] |
| 9 | EPL[3] | | 41 | EPL[14] | | 73 | EPL[24] |
| 13 | EPL[5] | | 45 | EPL[9] | | 77 | EPL[2] |
| 17 | **EPL[7]** ⚠ | | 49 | EPL[11] | | 81 | EPL[4] |
| 21 | EPL[16] | | 53 | EPL[13] | | 85 | EPL[6] |
| 25 | EPL[17] | | 57 | EPL[15] | | 89 | EPL[8] |
| 29 | EPL[18] | | 61 | EPL[21] | | 93 | EPL[10] |
| | | | | | | 97 | EPL[12] |

⚠ **Datasheet erratum.** Table 9-4 prints `EPL[6]` twice — at SBus 17 and at
SBus 85. The rest of the table is complete for EPL[1]–EPL[24], and the only
number missing is **7**, which sits exactly where the first `EPL[6]` is. Read
SBus 17 as EPL[7]. Stated here because a table that silently maps two
different EPLs to one number will send somebody after a dark port for days.

So a lane's SBus device id is `table[epl] + lane`, lane 0–3.

### What the bench adds, and what it does not

`SBUS_CFG` `0x0f000`, `SBUS_COMMAND` `0x0f001`, `SBUS_REQUEST` `0x0f002`,
`SBUS_RESPONSE` `0x0f003` — all present and idle after our boot sequence.
**live**

Writing a command and reading it back confirms the field layout exactly: a
command of device `0xfe`, op `0x22`, register 0 reads back with `22FE00` in
the low three bytes, untouched. **live**

Of the top byte: **bit 24 is EXECUTE** (it is what we set), **bit 25 behaves
as BUSY**, and **bit 28 appears after a transaction has been attempted**.
Candidate positions above 25 do not stick, so the field is narrow.
**derived**

### ⚠ `SBUS_CFG` bit 0 is a RESET, and the datasheet's wording hides it

The register "defines the reset state of the SBUS controller and the clock
ratio", and then §9.4 separately says "the clock ratio should be set to 4" —
which reads like an instruction to write 4. It is not. On this chip the
register holds **only bit 0**: write 2, 3, 4 or 5 and it reads back 0 or 1.

With bit 0 **set**, every command hangs with Busy stuck forever. **Clearing it
is what makes the bus work.** That cost an evening of assuming the clock ratio
was the problem.

### Result codes, and the control that found them

The controller returns 3 bits in `SBUS_COMMAND[28:26]`. What they mean was
established with a **negative control**, not by assumption: **live**

| device | non-zero of 64 regs | rc |
|---|---|---|
| 5 — EPL[1] lane 0 | **37** | **4** |
| 6 — EPL[1] lane 1 | **36**, and different values | **4** |
| 253 — SPICO, `0xFD` | 3 | 4 |
| 254 — controller, `0xFE` | 13 | 4 |
| **170, 200 — cannot exist** | **0** | **6** |

So **rc 4 is success and rc 6 is no-such-device**, and both of the datasheet's
reserved ids answer, which independently confirms the addressing.

⚠ **Do not test a transaction by whether the data is non-zero.** Register 0
reads `0x00000000` on *every* device on this ring, real or not. A first pass
that read register 0 across all 24 EPLs got 24 zeros and looked like a
completely dead bus; it was a completely working one. `fm_sbus_present()` asks
register 2 and judges on the result code.

### Where it stands

```
   24 of 24 EPL lane-0 SerDes answered (rc=4)
   control: device 200 does not exist -> rc=6  (as it should)
```

`fm6000-probe --lbus --sbus` prints that control every run, because a bus that
answers everything is not answering anything and the only way to tell is to ask
for something that cannot be there.

## Lane state, and the port-to-SerDes table, 2026-09-26

### Every lane is in the same unconfigured state

Read over the working SBus, all 96 Ethernet SerDes: **live**

```
   reg 0x0f  (rx_rdy_obs)          0x0a   bit 3 set, bit 0 CLEAR -> not locked
   reg 0x14  (rx_ib_sig_strength)  0x14   bit 6 CLEAR -> no signal detect
```

Identical on every lane, including the two whose cages hold optics with a far
end transmitting into them. That is the correct answer rather than a
disappointing one: nothing has run the lane-enable sequence, so no receiver is
powered and signal detect cannot mean anything yet. It also means **a lane
cannot be identified by its signal until it has been enabled** — the obvious
shortcut for confirming which SerDes belongs to port 1 is not available.

### The per-port table, from EOS's own FDL

`CotatiP4.fdl altaSfpPorts` carries, per front-panel port: the physical
("alta") port, EPL id, lane id, RX and TX polarity inversion, and TX drive,
precursor and postcursor. Polarity and equaliser settings are **board routing
facts** — they differ per port because the traces do — so they are data this
port needs and cannot compute.

⚠ It also **independently confirms two things measured here**, which is what
makes it trustworthy rather than merely convenient:

| | measured here | FDL |
|---|---|---|
| front panel 1 → physical | 40 | `alta 40` ✓ |
| front panel 2 → physical | 20 | `alta 20` ✓ |
| front panel 3 → physical | 41 | `alta 41` ✓ |
| cage register base/stride | `0x5010` / `0x10` | `xcvrOffset 0x5010, 0x5020, 0x5030` ✓ |

Three independent derivations of the port map now agree: the vendor agent log,
the SCD cage registers, and the FDL.

### ✅ Resolved: the FDL's `eplId` is not the datasheet's EPL number, and it does not need to be

The FDL puts port 1 on `eplId 14 lane 0`, and Table 9-4 makes EPL[14] SBus 41
— but the prior investigation's capture recorded port 1's SerDes as SBus
`0x49` (73), which is Table 9-4's EPL[24]. So the two numbering schemes are
permuted with respect to each other.

**They do not have to be reconciled**, because the lane arithmetic closes the
gap on its own. From the same capture: port 3's SerDes is `0x4a` (74), and the
FDL puts port 3 on `eplId 14 lane 1` — the *same EPL as port 1, next lane*.
`74 = 73 + 1` is exactly what "lane 1 of the EPL whose base is 73" means, so:

```
   FDL eplId 14  ->  SBus base 73      ports 1, 3, 5, 7   = lanes 0..3
   FDL eplId 16  ->  SBus base 69      ports 2, 4, 6, 8   = lanes 0..3
```

Both bases are genuine 4-lane groups in Table 9-4 (73 is EPL[24], 69 is
EPL[23]), so the two sources are consistent even though their labels differ.
Three facts agree — the capture's device ids, the FDL's lane indices, and
Table 9-4's grouping — and that is enough to address the lanes behind the
first eight front-panel ports without knowing the permutation for the other
44. **derived, from three agreeing sources**

⚠ The permutation for EPLs other than 14 and 16 is **still unknown**, so this
covers ports 1–8 and no further. That is enough: port 1 is the one with an
optic and a live far end, and it is the port to bring up first.

## What is and is not on Arista's GitHub, 2026-09-26

Surveyed before extracting anything further from the prior EdgeNOS work, on
the principle that an upstream component beats a local extraction.

### There is no FM6000 support, and that is now checked three times

`aristanetworks/sonic` (GPL-2.0) is the home of the open SCD driver and
initialisation library. It carries **no** `7150`, `raven`, `fm6000`, `cotati`
or `santarosa` anywhere in its 755 files. Its platform support starts at
clearlake/upperlake and goes forward. This board predates it.

### What IS there and is worth having

| | licence | use to us |
|---|---|---|
| `aristanetworks/sonic` `src/scd-*.c` | GPL-2.0 | the SCD driver: `scd-led.c`, `scd-gpio.c`, `scd-reset.c`, `scd-smbus.c`, `scd-spi.c`, `scd-uart.c`, `scd-mdio.c`, `scd-fan.c`. `scdsmbus` already derives from this one; the **LED** and **fan** drivers bear on two open gaps here |
| `aristanetworks/sonic` `arista/core/prefdl.py` | GPL-2.0 | the **PREFDL format** |
| `aristanetworks/swi-tools` | **Apache-2.0** | SWI and SWIX handling. We hand-build SWIs, so this is a permissively licensed implementation of something we already do |
| `aristanetworks/switch-interface-maps` | BSD-3 | has `DCS-7150S-52-CL_EosIntfMapping.json` — ⚠ but it is **empty**: `eth0 -> Management1` and `"EthernetIntf": {}`. No lane data |

### The PREFDL format, for when the device is found

A TLV list, each field a type byte: `0x01` Deviation, `0x02` MfgTime, `0x03`
SKU, `0x04` ASY, **`0x05` MAC**, `0x0a` HwApi, `0x0b` HwRev, `0x0c` SID, `0x0d`
PCA (12), `0x0e` SerialNumber (11), `0x0f` KVN (3), `0x17` MfgTime2, `0x00`
END; CRC32 (zlib) over the buffer; versions V2 and V3. **documented**

### ⚠ The two extra `0x50` responders are the PSUs, not the board

The earlier guess was wrong. Dumped, they are Emerson PSU FRU EEPROMs:
**live**

```
   accel 0 bus 4   EMERSON  DS460S-3-002 / DS460S-3-401  serial K192NL00SF1CZ
   accel 1 bus 0   EMERSON  DS460S-3-002 / DS460S-3-401  serial K192MB00XH1CZ
```

Two PSUs, both present, which agrees with the SCD's own presence bits. That is
real inventory NOSaic can report — model and serial per supply — and it is
**not** the board PREFDL.

### Where the PREFDL is not

Worth writing down, because the negative space is most of the search. **live**

| looked | found |
|---|---|
| flash filesystem, by name and by byte-searching every file for the MAC | nothing |
| kernel cmdline, `/sys/firmware` | `console=ttyS0,9600n8 panic=5`; only `memmap` |
| SCD SMBus `0x50` on accelerators 0–11 | 52 cages + the two PSUs |
| SCD SMBus `0x51`–`0x57` on accelerators 0–2 | SFP DOM pages only |
| host i2c-0 `0x50`/`0x51` | **DDR3 SPD** (`92 11 0b 02`, byte 2 = DDR3) |
| host i2c-1 `0x50`–`0x57` | all `0xff`; the i2cdetect hits were probe artefacts |
| host i2c-2 `0x4c` | a temperature sensor |

The host buses did confirm one thing: **`0x23` on i2c-1 is `thorn`**, which
makes i2c-1 the bus the earlier investigation called `/sb/1`.

### ✅ FOUND: the PREFDL is at i2c-1 address `0x52`

Answered by reading the vendor's own boot script out of the EOS SWI **on our
own flash**, with no EOS boot: the image is a plain ZIP whose `rootfs-i386.sqsh`
is **Stored** uncompressed, so it loop-mounts read-only straight from the file.

`/etc/rc.d/init.d/EosReadPrefdl` runs `genprefdl`, which dispatches to a
per-platform plugin. The one for this board is
`GenprefdlPlugin/Raven.py` — **raven** being this platform's codename — and it
reads the base prefdl from **`idseeprom --device=1.0x52`**: bus 1, address
`0x52`. (It then appends a second, CPU prefdl read from SPI flash via
`flashUtil`, prefixed `Cpu`. We do not need that one.)

`0x52` on **host i2c-1** — the PIIX4 bus that also carries `thorn` at `0x23` —
answers plain SMBus byte reads. The earlier sweep missed it by dumping `0x50`
and inferring the whole `0x50`–`0x57` range was dead from one address.

### The format is ASCII TLV, and the known-answer test passes

Not the binary TLV of Arista's `prefdl.py`, but the same field codes:
**2 hex digits of code, 4 hex digits of length, then the value.** **live**

| code | field | this chassis |
|---|---|---|
| `03` | SKU | **`DCS-7150S-52-CL`** |
| `0c` | SID | **`SantaRosa`** |
| `05` | MAC | **`444ca8315daa`** |
| `02` | MfgTime | `20170217015036` |
| `0b` | HwRev | `12.04` |
| `0a` | HwApi | `05.01` |
| `0d` / `04` | PCA / ASY | `0007922A0` / `0058122A0` |
| `09` | ⚠ board-specific | `{'AltaVdd':1.01,'AltaVdds':1.0}` |

`03 000f DCS-7150S-52-CL` is fifteen characters and `000f` is fifteen; `0c 0009
SantaRosa` is nine and `0009` is nine. The codes are Arista's own, from their
open driver. And the MAC is **exactly** the one `config/network.conf` has been
asserting by hand since 2026-09-23, which is the test that makes this the board
PREFDL and not another EEPROM that happens to contain text.

⚠ **Code `09` carries the Alta core rail voltages** — `AltaVdd 1.01`,
`AltaVdds 1.0`. That is board data the regulator investigation went looking for
in the CHL8228G, and it was in the prefdl the whole time. It is also why a
prefdl reader is worth more than just a MAC.



### `Cotati.hold` on the flash, identified

8.5 KB, and it is a **Silicon Labs Si5338 register map** in AN428 JumpStart
format — the clock configuration. It is the operator-supplied
`Cotati-Clock-0010.si5338` that the prior investigation's scoreboard lists as
**eliminated**, left behind on the flash. Not needed, and now not a mystery.

## A live lane against a dark one, 2026-09-26

Captured under EOS with four ports forwarding — Et1 to Et4, all 10GBASE-SR.
Ports 1 and 3 are EPL 14 lanes 0 and 1; ports 2 and 4 are EPL 16 lanes 0 and
1. Lanes 2 and 3 of each are dark, which gives live-versus-dark **inside one
EPL**, where the two lanes share their configuration registers. **live**

### The per-EPL gates, and the field width is settled

```
                 forwarding    ours, booted
   EPL_CFG_A      7e1d7899      0c7d7899
   EPL_CFG_B      00090033      00080000
   +0x03          00041082      00041041
```

`EPL_CFG_B` low byte `0x33` is **two ports set at four bits each** —
`Port0PcsSel=3` and `Port1PcsSel=3` — which is exactly the two live lanes on
that EPL. That settles the field width this page had marked UNKNOWN, and it
explains the earlier `0x00090003`: that reading was taken when only one port
was up.

So the selector is `EPL_CFG_B[4n+3:4n]` for lane `n`, and 3 is 10GBASE-R.

### PORT_STATUS is the first word of a lane's slot

`per-lane +0x00` reads **`0x000008c0`** on a live lane, which is the value
`FM6000_PORT_STATUS_UP` in regs.h has carried since before anyone knew where
the register was. Bit 11 is SerXmit. A dark lane reads `0x15`.

### The fifteen words that separate a live lane from a dark one

EPL 14, lane 0 (port 1, up) against lane 2 (port 5, empty cage):

| off | live | dark | |
|---|---|---|---|
| `0x00` | `000008c0` | `00000015` | **PORT_STATUS** |
| `0x04` | `00002a00` | `00001002` | |
| `0x10` | `2000033c` | `2000031c` | |
| `0x21` | `00402002` | `00001001` | |
| `0x26` | `00000001` | `00000000` | |
| `0x38` | `00000940` | `00000000` | the other value SPICO-RE records for a live port |
| `0x39` | `002a0281` | `00280280` | |
| `0x3a` | `c0000581` | `80000080` | |
| `0x3b` | `00000c83` | `00000803` | |
| `0x3c` | `000001fe` | `000001ee` | |
| `0x3e` | `00100f0f` | `00000000` | the per-lane field flagged earlier as where to look |
| `0x3f` | `00000060` | `00000780` | |
| `0x40` | `00003fff` | `00003fdf` | |
| `0x41` | `000004b0` | `00000000` | |
| `0x42` | `00000342` | `00000000` | |

Fifteen words. That is the target state for bringing a lane up, and it is the
diff the prior investigation spent two days narrowing to six.

### ⚠ Correction: the cage map was not triangulated, it was under-counted twice

This page claimed the cage mapping was confirmed three ways. It was not, and
the way it failed is worth keeping.

**Four modules are present, not two.** Under EOS the cage registers read:

```
   cage 1..4   0x00000180   module present
   cage 5..8   0x00000047   empty
```

and EOS has Et1 through Et4 connected. But the two NOSaic-side measurements
each saw only **two**: the SFP EEPROM scan found `0x03` at accelerator 2 buses
0 and 1 and nothing at 2 and 3, and the cage registers read `0x1E0` for cages
1 and 2 against `0x1DF` for every other. Those two agreed with each other and
with "Et1 and Et2 are up" — which was itself only true because I looked at the
OSPF adjacency rather than the interface list.

⚠ **Two measurements agreeing because they share a blind spot is not
corroboration.** Both were taken on a chip whose cages NOSaic has never
powered, and both under-reported by the same two.

So the cage-to-bus mapping was **not established** beyond cages 1 and 2. The
likely explanation was that an unpowered cage neither answers its EEPROM nor
reports presence.

### ✅ And that hypothesis is confirmed, which restores the mapping

After the chip had been through EOS once and back to NOSaic, the same two
measurements read differently — because EOS configured the cage registers and
a NOSaic boot does not reset the SCD: **live**

```
   before EOS ran     cages 1,2 = 0x1E0, everything else 0x1DF
                      EEPROMs at accel 2 buses 0 and 1 only

   after EOS ran      cages 1..4 = 0x180 (present), 5..52 = 0x187 (empty)
                      EEPROMs at accel 2 buses 0, 1, 2 and 3
```

`0x1DF` is the unconfigured state; `0x187` is configured-and-empty and
`0x180` configured-with-a-module, the difference being the three status bits
0-2. Scanning **all 52 cages** and **all seven accelerators** now finds
exactly four modules, in cages 1 to 4, at accelerator 2 buses 0 to 3.

So the mapping is sequential after all — panel port N at accelerator
`2 + (N-1)/8`, bus `(N-1)%8` — and it now rests on a measurement that could
have contradicted it rather than on two that shared a blind spot.

⚠ **What actually goes in NOSaic is the cage power-up**, because that is the
step EOS performed and NOSaic does not. Until it does, a module in a cage
this board has not powered is invisible to it: no EEPROM, no presence bit.
That is not a mapping problem, it is a missing initialisation.

## The EPL gates are not enough to light a lane, 2026-09-26

Tested directly, with all four cages populated and their far ends
transmitting — confirmed from the modules' own DOM rather than from the
vendor OS, which is a thing this board can now answer for itself: **live**

| port | module | rx | tx |
|---|---|---|---|
| 1 | CISCO-FINISAR FTLX8574D3BCL-CS | **−2.16 dBm** | −1.94 |
| 2 | CISCO-FINISAR FTLX8571D3BCL-C2 | **−2.01 dBm** | −2.61 |
| 3 | CISCO-ACCELINK RTXM228-551-C98 | **−2.89 dBm** | −2.93 |
| 4 | CISCO-FINISAR FTLX8571D3BCL-C2 | **−2.71 dBm** | −3.10 |

Three different module types, all healthy. Port 1 had −2.16 dBm arriving
throughout the attempt below.

Port 1 is EPL 14 lane 0. Booted the chip, then wrote the two per-EPL
registers to the exact values a forwarding chip holds:

```
   booted     CFG_A 0x0C7D7899   CFG_B 0x00080000   PORT_STATUS 0x15
   written    CFG_A 0x7E1D7899   CFG_B 0x00090033   chip alive
   after 5s   PORT_STATUS 0x15, +0x38 0x00000000    unchanged
```

Both writes took and read back. The chip stayed up. **And the lane did not
come up** — PORT_STATUS stayed at the dark value and never moved toward
`0x8c0`, with the far end transmitting into it the whole time.

So the EPL gates are **necessary but not sufficient**, and this rules out the
cheapest possible path to a link. The SerDes itself has to be configured
first, over the SBus, and only then do these registers mean anything.

⚠ This is a **stronger** negative than the prior investigation's, and the
difference is worth stating. There, setting `PcsSel` on a dark port turned
`SerXmit` 0→1 — but that was one dark port on a chip EOS had already
initialised, so its SerDes had been through the vendor's bring-up. Ours has
had no SerDes configuration at all. Setting the same gate on a chip where
nothing has touched the lane does nothing, which is consistent with their
finding rather than contradicting it, and it locates the missing work
precisely: the lane-enable algorithm, not the EPL registers.

⚠ Note also that writing a captured register value is **not** how this ships.
`EPL_CFG_A` differs from ours in six bits nobody has decoded, and writing the
forwarding value wholesale was an experiment to find out whether the gates
alone suffice. They do not, so the question is moot — but if they had, the
fields would still need decoding before any of it became driver code.

## The transmitter works, the receiver does not, 2026-09-26

```
   PORT_STATUS 0x00000015 -> 0x00000815
               SerXmit(11)=1   RxLinkUp(6)=0   HeartbeatOk(7)=0
               a forwarding lane reads 0x8c0: all three
```

Port 1 drives light under our own code. Eleven of twelve steps pass. The
receive half does not come up: signal detect never asserts, with **-2.16 dBm
arriving at the optic** the whole time, so the light is there and the SerDes
is not reporting it. **live**

### What moved it

Two corrections to the port, both mine, and both the same mistake -- taking a
value from the earlier superseded version of the sequence rather than the
corrected one:

- **PLL lock is reg 15 bit 3 alone**, not bits 0 and 3. Bit 0 does not set on
  this part, so the wait timed out for five seconds on a chip whose PLL was
  already locked.
- **A datapath enable, `reg 13 |= 0x11`**, sits immediately before the
  signal-detect wait and I dropped it in the rewrite.

And one bug of my own: **`EPL_CFG_B` needs a read-modify-write.** Four lanes
share it, so writing the PCS selector on its own left `0x00000003` against a
forwarding chip's `0x00090033` -- taking the other three lanes down and
clearing what the boot sequence put there. Fixing that is what set SerXmit.

### What has been ruled out for the receiver

All measured, all with light arriving: **live**

| tried | result |
|---|---|
| RX polarity inverted (`reg 7` bit 4) | no change |
| signal-detect threshold swept `0`-`63` (`reg 31[6:1]`) | no change, at any value |
| the EPL config words `+0x10`-`+0x13`, `+0x34`, `+0x35`, `+0x0c` | no change |
| `EPL_CFG_A` written to the forwarding `0x7E1D7899` | took, no change |
| `reg 13` datapath enable | passes, no change |

⚠ `reg 20` (signal detect) reads `0x14` **unchanged through all of it**, which
is itself a clue: a threshold sweep that moves nothing at either end of its
range suggests the receiver's analogue front end is not running, rather than
that it is running and seeing too little.

### Steps 17-18, the DFE: the mailbox answers, and it is the wrong suspect

Implemented in C (`fm_lane_dfe`) and run. The mailbox **is alive and does
respond to host driving with no SPICO firmware**, which confirms what
`SPICO-RE.md` reports: `0x2b` moves under toggling rather than sitting dead.
But it settles at `0x07` rather than the `0x03` a working fibre lane holds,
and RxLinkUp stays clear. **live**

⚠ **And it could not have been the cause.** DFE is decision-feedback
equalisation: it refines a signal the receiver is already recovering. It does
not create one. Chasing it was worth doing to close the last documented step,
but "no signal detect" was never a thing an equaliser was going to fix, and
noticing that earlier would have saved the detour.

### So the RX front end is the suspect, and there is a named candidate

The older version of the vendor sequence sets several things through **SPICO
interrupts** -- writes to the SerDes' own register `0x03` with the answer
polled from `0x04`. Among them, and conspicuous here:

```
   spico_int(dev, 0x2b, 1)     "rx termination"
```

Input termination is exactly the kind of analogue front-end setting whose
absence gives a receiver that reports nothing at any threshold, which is what
this board does. It is also exactly the kind of step that silently does
nothing when no SPICO firmware is answering.

The SBus scan does find device `0xFD` -- the datasheet's reserved SPICO id --
answering with three non-zero registers, so the controller is present. Whether
it is *running code* is a different question and has not been established.

### SPICO is confirmed not running, and that is not the problem

Posted a SPICO interrupt -- a write to the SerDes' register 3 with the answer
polled from register 4 -- and **register 4 never moved in 200 polls**. So
nothing is answering and every `spico_int` step in the vendor sequence,
including the rx-termination one, is a silent no-op here. **live**

That looked like the cause. It is not, and the prior investigation's own
measurements say why.

### ⚠ Signal detect never sets, even on a port that forwards

`SPICO-RE.md`, measuring a working fibre port and a non-working copper one on
a stripped no-SPICO boot:

> PLL lock and signal detect are IDENTICAL on both ports — reg `0x0f` = `0x3f`
> and reg `0x14` = `0x14` on dev `0x45` (et2) and `0x49` (et1).

**`reg 0x14` = `0x14`, bit 6 clear, on the port that carries traffic.** So the
wait this port had on that bit was waiting for something that does not happen,
and the threshold sweep that "moved nothing at either end of its range" moved
nothing because there was nothing to move. That is the third value taken from
the superseded version of the sequence rather than the corrected one, after
the PLL bits and the missing datapath enable.

Our lane now reads `0x0f` = `0x3f` and `0x14` = `0x14`: **identical to a
working fibre port.** The SerDes is not the problem.

### The real acceptance test, and where we actually are

```
   their et1, fibre, forwarding   PORT_STATUS 0x000008c0   LANE_STATUS 0x00000940
   their et2, copper, no lock     PORT_STATUS 0x00000815   LANE_STATUS 0x00000000
   ours,      fibre               PORT_STATUS 0x00000815   LANE_STATUS 0x00000000
```

LANE_STATUS is the lane's own `+0x38`, which this page's live-versus-dark diff
had already shown as `0x940` on a live lane without knowing what it was called.
The port code now waits on that instead of on signal detect, and it reports an
honest failure rather than a misleading one.

⚠ **We are in the copper port's state while holding a fibre port's optics.**
That is the shape of the remaining problem. Their et1 locked with no Intel
firmware, so fibre demonstrably can -- but it was running the whole of
EdgeNOS: parser, CM, MOD, scheduler, the lot. Ours has had Table 4-1, the lane
enable and the two EPL gates and nothing else. Receiver lock is a PCS
function, and the PCS does not run on a fabric nobody has configured.

So the next question is not "what else does the SerDes need" -- the SerDes
matches a working one register for register. It is **how much of the rest of
the chip has to be up before the PCS will lock.**

## Running the scaffold: where it stops, exactly, 2026-09-26

The scaffold exists to get a lane to link so block-by-block replacement has an
oracle. Two ways of running it were tried and both stop in the same kind of
place. **live**

### The 41 blocks, run in order

Blocks 1-5 -- `cminit` `safinit` `ffuinit` `l2linit` `parserinit` -- run
clean. **Block 6, `modinit`, takes the chip off the bus.**

That is what the file predicts: *"TABLE ONLY, deliberately: emits the 3855
registers written exactly once and leaves the 306 multi-write control
registers in the replay."* A block built as a substitution inside a sequence
leaves its own block half-configured when run alone.

### The whole replay, run directly

`fm6000_fullreplay` retargeted to the SCD local bus. It is runnable in
principle -- the replay's highest address is `0x3fc7ff`, so all 373,345
writes fit inside BAR1's 16 MB window, and it touches neither of the two
words measured fatal here. It has its own off-bus detection and reports
precisely:

```
   OFF-BUS at line 16384 (0x145ea4 <- 0x00000000)
   ABORTED: 16384 ops, mmio=14365 sbus=673 timeouts=0, PIN=0x00000000
```

The context is a **sequential zero-fill**, `0x145e00` upward, and it dies 164
words into a 304-word run -- having already completed an identical 304-word
run at `0x145c00`. A uniform fill failing partway through, after its twin
succeeded, is not a property of that address.

### What the replay's own fills say about memory

Extracted every run of consecutive words written zero, as a way of finding
the memories without guessing. There are **four**, and they are small:

```
   0x01f080 .. 0x01f23f    448 words
   0x113040 .. 0x11313f    256 words
   0x145c00 .. 0x145d2f    304 words
   0x145e00 .. 0x145f2f    304 words     <- died 164 words in
```

So these are table clears, not the big ECC sweeps Table 4-1 step 12 is about.
Our step 12 fills STATS and the replay does not fill it at all -- it is
already done by the time this capture starts.

### ⚠ Which is the point: the replay assumes a chip we have not built

It was recorded from EOS's boot, from a state EOS had already established.
Ours is Table 4-1 and nothing else. The prior work ran this file from
EdgeNOS's own `init-m1`, whose `fm6000_preboot` includes a **BIST and memory
repair pass** (`fm6000_bist_memory_init`) that this port does not do at all --
Table 4-1's step 9 issues the boot controller's bank-repair *command*, which
is not the same thing.

So the next move is not to bisect `0x145ea4`. It is to reproduce the preboot
the replay was captured after. Until then, both scaffold routes fail in the
same way and for the same reason.

## The memory BIST works; the replay path is a chain, 2026-09-26

### ✅ BIST and memory-controller configuration, ported and passing

`datapath/fm6000/bist.c`. Configures the chip's memory controllers and runs
the defect/repair march. **live**

```
   controllers configured  yes
   march completed         yes after 29 ms
   BM_ENGINE_STATUS        0x00000000
   result registers set    0 (want 0)
```

This is not Table 4-1 step 9. That step asks the boot controller to apply the
fusebox's recorded repairs; this establishes the controllers those memories
are reached through at all.

⚠⚠ **The controller writes are paced, and this hazard hangs the HOST.**
Writing the `0x1d200`-`0x1d6ff` block back to back hard hangs the machine
issuing the writes — not an off-bus chip that a reset pulse recovers, but a
box that needs its power cycled. Pacing is **on by default** here, 50 µs,
unlike the original where it defaulted to none behind an environment
variable. A default that can wedge the host is not a default.

### ⚠ "OFF-BUS at line N" is where the tool CHECKED, not where it broke

The replay tool reported the fault at line **16384** twice, which is exactly
2¹⁴ and should have been suspicious immediately. Running the first 16,000
lines alone printed `DONE` — and `PIN=0x00000000`. The chip had died during
them and the tool only noticed at its periodic check.

So `0x145ea4`, recorded here earlier as the fault, was the **detection
point**. Bisecting properly, with verified recovery, gives the real one:

```
   first fatal line: 2271      0001f000 <- 00000001
```

`0x1f000` is the CRM block's control register, and that write is the **CRM
launch** — the datasheet's first option for step 12's memory initialisation,
where this port chose the second, a software fill.

### ⚠ And it is a chain, not a blocker

Removing all 129 CRM-launch writes lets the replay past line 2271 and it dies
further on, at a different address. Each fault fixed reveals the next.

That is the honest read on this route. The replay was captured from a chip
EOS had already prepared, ours is prepared differently, and every place the
two states differ is a separate fault to find. It is not one missing step.

⚠ Note also that the BIST — the leading hypothesis for this — **did not
change the outcome**: the replay dies in the same place with the memory
controllers configured as without. Worth having anyway, and not the answer
here.

## Four ports transmit, and a mapping method that does not work

### All four live ports reach SerXmit

Ports 1-4, run through the lane enable in turn: **live**

```
   port 1   PORT_STATUS 0x00000815   SerXmit=1
   port 2   PORT_STATUS 0x00000815   SerXmit=1
   port 3   PORT_STATUS 0x00000815   SerXmit=1
   port 4   PORT_STATUS 0x00000815   SerXmit=1
```

Identical across **two EPLs and four SerDes devices** (`0x49`, `0x45`, `0x4a`,
`0x46`), which is what validates the port table and the device mapping rather
than one lucky lane. The receive half is still dark on all four.

### ⚠ A sweep that cannot work, and why it looked like it could

The FDL numbers EPLs one way and Table 9-4 another, and only two of the
twenty-four pairings are pinned — EPL 14 to SBus 73 and EPL 16 to SBus 69.
Two points do not determine a permutation.

The idea was to ask the hardware: enable lane 0 of one SBus address, then look
at all 24 EPL blocks and see which one's PORT_STATUS moved. Twenty-four trials
for the whole map.

**It does not work.** PORT_STATUS only moves when the SerDes half *and* the
EPL half are both configured, so pointing the EPL writes at a fixed block
makes the test succeed exactly when the guess was already right. The first run
"identified" EPL 14 from SBus 73 — with 14 hardcoded — and then found nothing
for SBus 69, which is the correct answer to a question that was really "is 69
paired with 14".

A test that needs the answer to ask the question is not a test.
`fm6000-probe --try-pair EPL SBUS` now takes both and confirms or refutes one
pairing; both known ones check out.

### ⚠ Then I built the sweep anyway, twice, and both were wrong

**In one process:** produced a complete, clean-looking table of all 24 EPLs.
It is wrong. It puts EPL 16 on SBus 49 when the known pairing is 69 — which
it gave to EPL 24. The cause is that it never reboots: by EPL 16 it has
enabled a dozen SerDes, and with that much state an EPL's gates alone move
its PORT_STATUS, so whichever candidate is tried first gets the credit.

**The control that proves it**, from a clean boot each time:

```
   EPL 16 + SBus 69   moved 0x015 -> 0x815
   EPL 16 + SBus 49   nothing
   EPL 16 + SBus  5   nothing
   EPL 16 + SBus 97   nothing
```

So the signal is specific **from a clean chip and only from one**.

**With a reboot per trial:** correct in principle and impractical. I put the
recovery inside the candidate loop, making it 576 reboots rather than 24, and
it ran for forty minutes, reached EPL 12, then wedged the box hard enough
that three recovery attempts failed and the harness aborted.

⚠ The abort is the part that worked. It stopped rather than reporting, and
the eleven pairings it did collect are **not recorded here** — there is no
known pairing among EPLs 1-11 to check them against, so they are unvalidated
and an unvalidated table is what this whole section is about not producing.

The watchdog then power-cycled the box and NOSaic came back by itself, which
is the second time that recovery path has earned its place.

**The permutation remains unknown for 22 of 24 EPLs**, and a third attempt at
sweeping is not the next thing to try.

## Confirmed on the bench, 2026-09-22

Unit A was powered from cold (`apc1` outlet 6, named `7150S-unitA`) and booted
EOS 4.16.8M unattended. Read off its own console at 9600 on panel 4, so these
are this chassis rather than the inventory record:

```
Aboot 2.1.0-3037058
Booting flash:/EOS-4.16.8M.swi
Model: DCS-7150S-52-CL
Serial Number: JPE17060680
System RAM: 3978148 kB
Flash Memory size:  1.5G
sw7150-lab login:
```

Cold power-on to login prompt is a little under four minutes. Two things in the
boot worth noting: **"Starting NorCal initialization"** is the step that brings
the SCD up and releases the FM6000 — the board-level init this port has to
replace — and Aboot offers its `Control-C` shell without a password.

The chassis label reads `-CL-F`, `show version` reports `DCS-7150S-52-CL-F`,
and prefdl's `SKU` is `DCS-7150S-52-CL`.

### prefdl, read from the running box

```
PCA: PCA0007922A0            SerialNumber: JPE17060680
SKU: DCS-7150S-52-CL         HwRev: 12.04
SID: SantaRosa               HwEpoch: 01.00
MacAddrBase: 44:4c:a8:31:5d:aa
FdlVariables: {'AltaVdd':1.01,'AltaVdds':1.0}
CpuSKU: BodegaCard           CpuSerialNumber: JPG17060379
```

Three of those are needed by this port and are not guessable:

- **`HwEpoch: 01.00`** — so `aboot_max_hwepoch: "1"` is correct, now measured
  rather than inherited from the sibling board.
- **`MacAddrBase: 44:4c:a8:31:5d:aa`** — the board's real MAC. It is not in the
  NIC, and it is *not* what a kexec from EOS leaves behind.
- **`FdlVariables: {'AltaVdd':1.01,'AltaVdds':1.0}`** — the FM6000's core supply
  voltages. These are **per-board data**, so whatever brings the Alta rails up
  has to read them from prefdl rather than carry a constant. The sibling
  7050SX2's board notes flag the same thing as outstanding work there.

### Flash space

```
/dev/sda1   1.5G  978M  511M  66%  /mnt/flash        (2026-09-22, after cleanup)
```

**511 MB free.** It was 59 MB until twenty-five EdgeNOS `.swi` builds — the
`0.3.0-alpha*` series plus `m1-*`, `memfix*`, `ourparser` and `special`, about
450 MB — were removed on 2026-09-22 with the owner's go-ahead. They are
reproducible from the EdgeNOS build tree; nothing else was touched.

What is left:

| Size | What | |
|---|---|---|
| 416 MB | `EOS-4.16.8M.swi` | **keep** — the only way back, and the last EOS supporting this platform |
| 20 MB | `fm6000_boot_regtrace.log` | reverse-engineering oracle material |
| 13 MB | `m1-warm-bzImage` | |
| ~530 MB | a long tail of RE text — `fwd*.txt` at ~6.5 MB each, `snap-*.txt` at 2.3 MB, register and stats dumps | |

So there is roughly another half gigabyte reclaimable if the traces are archived
off the box, which would be the move if A/B slots turn out not to fit in 511 MB.
They are evidence rather than clutter, so they stay until that is needed.

`boot-config` still reads `SWI=flash:/EOS-4.16.8M.swi`.

## The board

```
        ┌──────────────────────────────────────────────────────────────┐
        │  AMD Family-10h embedded, x86_64, 3978148 kB                 │
        │  00:18.0-4  HyperTransport config  1022:1200..1204           │
        └───────────────┬──────────────────────────┬───────────────────┘
                        │                          │
             RS780 northbridge            SB700/SB800 southbridge
             1022:9601                    │
                        │                 ├─ 00:14.6  BCM5785 GbE  14e4:1699
        ┌───────────────┴──────────┐      │             └─ BCM50610 PHY, RGMII
        │                          │      │                └─▶ ma1  (mgmt)
   PCIe port 0               PCIe port 5  ├─ 00:11.0  SATA 1002:4390
   00:04.0 (1022:9604)       00:05.0      │             └─▶ USB DOM /dev/sda
        │                    (1022:9609)  │                  sda1 FAT32 /mnt/flash
        │                          │      ├─ 00:12.x/13.x USB  1002:4396/4397
        ▼                          ▼      ├─ SMBus   1002:4385
  ╔═════════════════╗     ┌─────────────────────┐
  ║ 02:00.0         ║     │ 04:00.0             │  LPC 1002:439d
  ║ FM6000 "Alta"   ║◀────│ SCD FPGA "Saguaro"  │
  ║ 8086:155b       ║ held│ 3475:0001           │
  ║                 ║  in │                     │
  ║ BAR0 0xe2000000 ║reset│ BAR0 0xe1000000 256K│──▶ LEDs, SFP TX_DISABLE,
  ║      32 MB      ║     │ BAR1 0xe0000000 16M │    resets, watchdog,
  ╚════════╤════════╝     └─────────────────────┘    SMBus to cages + sensors
           │
           ▼
   52 × SFP+ 10G, direct serdes, no external PHYs
```
*(live — `lspci -v` on the running unit, 2026-09-22. The 16 MB second SCD BAR
is real and unexplained.)*

### Who holds what in reset

**Measured 2026-09-23, on this board, from the Aboot shell on a cold chip.**
This was `derived` before; it is now the strongest kind of live evidence,
because it was tested by releasing the resets and watching what happened.

```
  Aboot shell, cold board:
    /sys/bus/pci/devices/     0000:04:00.0 present   (SCD)
                              0000:02:00.0 ABSENT    (FM6000)
                              0000:00:04.0 present   (the bridge it lives behind)

    SCD reset block 0xe1004000 = 0x00000106     bits 1, 2 and 8 HELD
    SCD reset status 0xe1004020 = 0x00000000
    SCD version      0xe1000100 = 0x00217361     as predicted
```

⚠ **Unimplemented reset bits read as ZERO on this board.** The sibling 7050SX2
reads `0xfffffffc` with its ASIC running, because there unimplemented bits read
as *one* — and its driver's comment warns that taking another platform's bit
number would be silently fatal. The polarity convention is inverted here, so
that warning applies in reverse: on this board a bit reading 0 tells you
nothing, and only the three set bits are real.

### Releasing the resets is necessary and NOT sufficient

The experiment, run from Aboot with `devmem`:

```
  devmem 0xe1004010 32 0x2     ->  0x4000 = 0x00000104     bit 1 cleared
  devmem 0xe1004010 32 0x4     ->  0x4000 = 0x00000100     bit 2 cleared
  devmem 0xe1004010 32 0x100   ->  0x4000 = 0x00000000     bit 8 cleared
  echo 1 > /sys/bus/pci/rescan                  ->  02:00.0 still absent
  echo 1 > .../0000:00:04.0/rescan              ->  02:00.0 still absent
```

So the **clear port at `0x4010` works on this board** — the reset register moved
exactly as asked, three times — and with **every reset bit released the FM6000
still does not appear on the PCI bus.** A bridge-level rescan does not find it
either, so this is not the kernel having enumerated an empty bus at boot.

**Something in EOS's "NorCal initialization" does more than release resets.**

The leading hypothesis is **power**: prefdl carries `AltaVdd 1.01` and
`AltaVdds 1.0`, which are this board's ASIC core rails, and the sibling
7050SX2's notes already say that reading prefdl "gates the ASIC core voltage".
A chip whose core supply has not been programmed would behave exactly like
this — out of reset, and not training a PCIe link. The regulator is presumably
on the SCD's SMBus.

That is a hypothesis with evidence behind it and it is not yet tested. What is
established is the negative: reset release alone is not what brings this chip
onto the bus.

### The cold-versus-warm SCD difference

The SCD is reachable in both states — present in Aboot on a cold board, present
under EOS on a warm one — so the same diff that identified the FM6000 registers
works one level down, and needs no chip on the bus. Both halves taken
2026-09-23 with `spike/scd-dump.c`, read-only, over `0x00000`–`0x17fff`.

**The first result is a negative, and it is the useful one:**

```
  reset block 0x4000    cold 0x00000106      warm 0x00000000
```

Warm is **exactly the state we produced by hand** by clearing bits 1, 2 and 8 —
and the chip did not enumerate. The reset register is not where the difference
lies.

What did change, outside the interrupt-mask block: the watchdog `0x0120`
(`41f41770` → `c3e8157c`); `0x0190`–`0x019c` (zero → `1374 13dc 12dc 132c`);
`0x0210`–`0x021c` (zero → `ffffffff`); `0x3400`; `0x3800`–`0x381c`; and the LED
and transceiver block at `0x5000`.

⚠ **The diff may not contain the answer.** If the missing step is an **SMBus
transaction** — writing a core voltage into a regulator — it leaves no trace in
a register diff at all, because the controller returns to idle when the
transfer completes. A small diff is not evidence that little happened.

### The four devices, and where each one lives

Read out of the vendor's own FPGA plugin on the box (`FpgaPlugin/Bodega.py`,
readable Python) and cross-checked against the register dumps:

| Device | Part | Reached by | Version reg | This board |
|---|---|---|---|---|
| **saguaro** | Xilinx XC6SLX45T | PCI `0000:04:00.0` — **it is the SCD** | BAR0 `0x100` | `0x00217361`, v33 |
| **prickle** | Altera EP2C20F484C8N FPGA | SCD register | `0x160 >> 16` | `0x002a0000` → **42** |
| **quill** | Lattice XO-1200 FG256 | SCD register | `0x170 >> 16` | `0x00550000` → **85** |
| **thorn** | Altera **EPM240**F100C5 CPLD | **SMBus `/sb/1`, address `0x23`** | register `1` | **34** |

Every version matches `NorCalInit`'s log line for line, which is what confirms
the mapping rather than merely suggesting it — and `0x160`/`0x170` were already
sitting in the warm dump with nothing to attach them to.

**`thorn` is the lead.** It is not on the SCD at all: it is on the **host
southbridge's SMBus**, reachable from the CPU with the SCD uninvolved. An
EPM240 is a 240-element MAX II CPLD — the part a board uses for **power
sequencing**, not datapath logic. That makes it the best candidate for what
holds the Alta unpowered, and it sits on a bus NOSaic reaches with a stock
`i2c-piix4` on the SB700.

⚠ Reading it from under a running EOS returned `Smbus transaction failed` where
`NorCalInit` read it fine at boot — the bus is presumably held by the vendor's
platform agent. Do this from our own image, or from Aboot.

### thorn register 5, cold versus warm

Read with `spike/thorn-read.c` in both states, 2026-09-24 — cold from a
RAM-booted NOSaic, warm from EOS, the **same static binary** in both so the
comparison is not across two tools.

```
  thorn, SMBus 0x23 on the PIIX4 adapter that answers (NOSaic /dev/i2c-1)

  reg    cold   warm
   0     0x00   0x00
   1     0x22   0x22     version 34, as NorCalInit reports
   2-4   0x00   0x00
   5     0x01   0xa1     ◀── THE ONLY DIFFERENCE
   6     0x07   0x07
   7-23  0x00   0x00
```

**One register, two bits.** `0xa1` against `0x01` is bit 7 (`0x80`) and bit 5
(`0x20`) set on a board whose ASIC is running and clear on one whose ASIC is
not. Every other register in the first 24 is identical.

That is the first thing found anywhere on this board that distinguishes the two
states and is not downstream of the ASIC already being up.

**What is not known is whether those bits are cause or effect.** A power
sequencer's register file holds both: control bits that enable rails, and status
bits that report them good. If bit 7 and bit 5 are status, writing them achieves
nothing; if they are control, writing them is what turns the Alta on. Nothing
here distinguishes those yet, and the difference matters because the experiment
that settles it is a **write to a power sequencer**.

A wider read settles where to look: across all **256** registers, warm, only
three are non-zero — `1` (version `0x22`), `5` (`0xa1`) and `6` (`0x07`). There
is no more-obviously-control register elsewhere in the file. Register 5 is the
only candidate.

### The board's own power-up sequence, read from the vendor's board module

**This supersedes the thorn-write experiment below, and it is why that
experiment should not be the next thing anyone runs.**

`DosBoard/SantaRosaPca` is the vendor's description of *this* board. It is
compiled Python, but the runtime is on the switch, so it can be introspected
rather than decompiled: every function's `co_names` and `co_consts` are
readable, which gives the components, their addresses and the order a routine
touches them. `spike/introspect.py` does that. **No vendor code is copied here;
what follows is the board's layout as it describes itself.**

The board carries two power devices this port had never heard of:

| Name | Part | Address | |
|---|---|---|---|
| `ir` | **CHL8228G** | `i2cSmbusAddress 0x30`, `smbusAddress 0x70` | dual-rail digital PWM controller — `loop1Vid`/`loop2Vid`, `vmaxRail1`/`vmaxRail2`. **Two rails, matching `AltaVdd` and `AltaVdds`** |
| `dpm` | **UCD90160** | `0x4e` | TI power-supply sequencer and monitor |

And `SantaRosaP5.initialize()` touches them in this order:

```
   scd.initialize          ──  the SCD first
   altatemp
   ir      + 'vidMode'     ──  THE VOLTAGE CONTROLLER, put into VID mode
   ucd                     ──  the power sequencer
   hal.resetSet            ──  resets ASSERTED
   rd / rpt / alta / sol / wr
   hal.resetClear          ──  resets RELEASED
   repeaters
   max6658.setup
```

**The regulator is programmed before the resets are released.** This port
released the resets and never touched `ir` or `ucd` at all, which is exactly
consistent with what was measured: reset bits clear, chip still absent.

⚠ **How much to trust this.** `co_names` is the order in which a function looks
names up, which tracks call order closely but is not a decompiled listing —
treat the sequence as strong evidence of shape, not as a transcript. The
component addresses come from `co_consts` beside their names and are firmer.

### Where the two power devices actually sit

Neither is on the host SMBus — probing `0x30` and `0x70` across every
`/dev/i2c-*` got no answer. They are behind the **SCD's SMBus accelerators**,
and a bounded scan found both exactly where the board module said they would
be:

| Path | Device | |
|---|---|---|
| `/scd/1/1/0x70` | **CHL8228G** (`ir`) | accelerator 1, bus 1 — the Alta core rails |
| `/scd/0/1/0x4e` | **UCD90160** (`dpm`) | accelerator 0, bus 1 — the sequencer |

The regulator's first 48 registers, read warm (ASIC running), as the baseline
the cold read will be diffed against:

```
 00 09  01 08  02 26  03 80  04 07  05 0c  06 08  07 2e
 08 20  09 b3  0a 0a  0b a6  0c 00  0d 00  0e f1  0f 65
 10 00  11 00  12 00  13 00  14 00  15 00  16 00  17 00
 18 00  19 00  1a 00  1b 70  1c 0b  1d 08  1e b0  1f c0
 20 c1  21 c0  22 c0  23 15  24 04  25 04  26 06  27 01
 28 10  29 12  2a 37  2b 00  2c 00  2d d9  2e 01  2f 14
```

### The device at `0x70` is the **Si5338 clock**, and the dump missed the part that matters

Settled by the vendor's own Si5338 register table
(`DosComponent/Si5338/Si5338Hal`), which gives:

| Si5338 register | Offset | Our dump |
|---|---|---|
| `revisionId` | `0x00` | `0x09` |
| `pllMask` | `0x06` | `0x08` |
| **`i2cAddress`** | **`0x1b`** | **`0x70`** |
| `refClock` | `0x1c` | `0x0b` |
| `phaseCtrl` | `0x1d` | `0x08` |
| `rDivider[0..3]` | `0x1f`–`0x22` | `c0 c1 c0 c0` |

**The device reports its own I²C address as `0x70` at offset `0x1b`** — the
address it answers on. A CHL8228G has no reason to hold `0x70` there. This is
the Si5338.

So the earlier reading was doubly wrong: the device was misnamed, *and* the
conclusion drawn from the name was the wrong one of the two. It is the clock
that reads identically cold and warm, and **the CHL8228G has never been read at
all.**

#### And the frequency registers are identical too

Re-read over the range that actually programs the clock — `0x30`–`0x70`, the
multisynth `msCtrl`/`msCoef`/`msnCoef` block — cold from a RAM-booted NOSaic
and warm from EOS:

**All 65 registers identical.** The multisynth configuration on a cold board is
the same as on a running one, so the Si5338 comes up programmed and
*"Programming clock for sid"* is not doing anything this port is failing to do.

Still unread: `0xda`–`0xf6` (`los`, `outputDrive`, `fcal`, `softReset`) — the
thermal restart storm ate that half of the output twice. `outputDrive` at
`0xe6` is the one worth having, since a configured clock with its outputs
disabled would look exactly like this. Warm it reads `0x00`.

And `0xda`–`0xf6` is identical too, `outputDrive` at `0xe6` included — `0x00`
cold and `0x00` warm — as is the page register at `0xff`.

**The clock hypothesis is closed.** Every register read on the Si5338 —
`0x00`–`0x2f`, `0x30`–`0x70`, `0xda`–`0xf6`, `0xff` — is byte-identical between
a board whose ASIC is dark and one whose ASIC is forwarding. The clock comes up
fully configured and is not what the board does differently.

### The CHL8228G, found at last — and the rails are already on

The regulator was never missing, only mis-scanned. The board class carries the
whole SMBus map as `accel.bus`:

```
   alta 0.2    dpm 0.5    ir 0.3     osc 1.1    psu1 1.0
   psu2 0.4    repeater 1.2    scd 0.1    security 0.6    switch 0.0
```

`osc` is `1.1`, which is exactly where the Si5338 was found — so the map is
confirmed by something already measured. **`ir` is `0.3`**: the CHL8228G is at
`/scd/0/3/0x70`, and `dpm` (UCD90160) at `/scd/0/5/0x4e`.

Earlier scans missed it for a reason worth remembering: **they probed register
0, and on a PMBus device command `0x00` is `PAGE`, which does not answer a
read.** A scan that decides "nothing here" from register 0 alone will walk past
every PMBus part on the bus.

It answers on its implemented commands, and they decode:

| PMBus | Cold | Warm | |
|---|---|---|---|
| `0x01` OPERATION | **`0x88`** | **`0x88`** | bit 7 set — **the unit is ON in both** |
| `0x19` CAPABILITY | `0xa0` | `0xa0` | |
| `0x20` VOUT_MODE | `0x15` | `0x15` | linear, exponent −11 |
| `0x78` STATUS_BYTE | `0x00` | `0x02` | |
| `0x8c` READ_IOUT | **`0x12`** | **`0x1d`** | **current is flowing cold, and more of it warm** |

**The rails are up on a cold board.** `OPERATION` has its enable bit set in both
states, and `READ_IOUT` is non-zero cold — the regulator is not merely enabled,
it is delivering current. The increase to warm is the ASIC drawing more once it
runs, which is what you would expect of a chip that is powered and idle.

So the power hypothesis is closed too, and closed in the most useful way: not
"we could not find a difference" but "the thing is measurably on".

### The root port sees nothing: no link, no presence

The RS780 root port at `00:04.0`, read in both states. Warm, via `lspci -vv`:

```
LnkSta:  Speed 5GT/s, Width x4, DLActive+
SltSta:  PresDet+
```

Cold, from the raw config space (PCIe capability at `0x58`, so `LnkSta` is at
`0x6a` and `SltSta` at `0x72`):

```
0x58: 10 a0 42 01  20 80 00 00  10 09 00 00  42 0c 30 f7
0x68: 00 00 00 11  60 00 24 00  08 10 00 00  18 00 01 00

   LnkCap = 0xf7300c42        the port can do 5GT/s x4
   LnkSta = 0x1100            speed 0, DLActive = 0   ← THE LINK IS NOT UP
   SltSta = 0x0000            PresDet = 0             ← NO DEVICE DETECTED
```

**Cold, the root port does not even detect a device**, let alone train a link.
Warm it is `x4` at 5GT/s with the data link layer active.

That is the clearest statement of the problem yet, and it rules out a whole
class of explanation. The host side is fine — the port is capable, configured,
and given a bus number and a window by the kernel. Nothing is wrong upstream of
the endpoint. **The FM6000 simply is not driving its PCIe receivers.**

Which fits the one piece of the datasheet that has been sitting unexamined.
`SOFT_RESET` holds the chip's own PCIe block at reset by default, and Table 4-1
step 11 reads *"If PCIe is used, BOOT ROM must setup PCIe SerDes and take PCIe
out of reset"*. That is work done **inside the chip, by its boot ROM**, before
any host can talk to it — and it is the step nothing on this board has been
shown to perform.

So the question has changed shape. It is no longer "what on the board must be
programmed first", which three measured negatives have answered with *nothing*.
It is **"why does the chip's own boot sequence not bring its PCIe up"** — which
is about `BOOT_MODE` straps, the serial EEPROM the boot controller reads, and
what `CHIP_RESET_N` is actually wired to.

### The chip boots its PCIe from an SPI ROM, and that is the whole problem

§7.2 of the datasheet states it outright:

> *"At power up, the PCIe differential pairs are in a reset state and the PCIe
> interface is inactive until the PCIe differential pairs are initialized. This
> requires the PCIe block of the switch to be initialized via an external ROM,
> both SPI Flash and I²C EEPROM are supported and can be selected by boot mode
> set via the GPIO[9..7] settings."*

**The host cannot do this.** There is no order of operations from the CPU that
brings up a PCIe link the CPU can only reach over PCIe. An external ROM does it
or nothing does.

Which boot mode this board straps is now established from two independent
directions, rather than assumed:

- the vendor's register header puts `PIN_STRAP_STAT` at `MGMT2 + 0x021` —
  `0x1c021`, exactly where we read it — with `bootMode0/1/2` at bits 7, 8 and 9;
- this chassis reads `PIN_STRAP = 0x208`, so bit 9 is set and bits 7 and 8 are
  clear: **`BOOT_MODE = 0x4`**;
- and Table 3-1 gives `0x4` as *"Boot from SPI serial boot ROM, image address
  pointer at offset 0."*

So the FM6000 here boots from an **SPI flash**, and that flash holds the image
that initialises its PCIe. The chip is not failing to be powered, clocked or
released — all three are measured — it is failing to *read its boot ROM*, or
never being asked to.

### The link stays dead after a real reset release — measured

The release is not hypothetical on a booted NOSaic: the `asic-release` service
runs `nosaic platform release-asic` every boot, through the SCD driver, and its
failure (`context deadline exceeded`) is it writing the reset bits and then
waiting for a device that never arrives. `NOSAIC-IRQ 0000:02:00.0 is not on the
bus` in the same boot says the same thing from the other side.

Reading the root port **after** that release:

```
   0x68: 00 00 00 11   LnkSta = 0x1100 — DLActive still clear
```

So this is now measured rather than inferred: **the resets are released by our
own driver, and the link still does not train.** The earlier reading could be
read as "nobody released it yet"; this one cannot.

### And the SCD's SPI block is not an arbiter

The block at `0x7900` decodes to three registers — `spicmd` at `+0x00`
(`data`, `csEnd`, `intrWhenDone`, `recordSpiOp`), `spiread` at `+0x10`
(`data`, `valid`), `spictrl` at `+0x20` (`readFifoCnt`, `writeFifoCnt`, `intr`,
`ovfl`, `unfl`).

That is a plain SPI master with a FIFO. **There is no owner, grant or mux field
anywhere in it**, so the "SCD and FM6000 share a flash and something arbitrates"
idea is not supported by the hardware as described. The FM6000 has its own SPI
pins — `GPIO[3,4,5]` for `SPI_CS_N`/`MOSI`/`CLK` and `GPIO[6]` for `MISO` — so
its boot flash is most likely its own, wired directly, with nothing to contend
for.

Which sharpens the question again rather than answering it: if the chip has its
own flash and its own pins, and the board has released its reset, then either
the strap latch never happened, or `CHIP_RESET_N` is not what the SCD's `alta`
bit drives.

### When the chip actually appears under EOS

From the vendor's own boot, `dmesg`:

```
  t=34.7   scd module installed / scd 0000:04:00.0: scd detected
  t=156.6  scd 0000:04:00.0: scd_finish_init
           scd 0000:04:00.0: scd device initialization complete
  t=173.6  pci 0000:02:00.0: [8086:155b] type 00 class 0x020000    ← APPEARS
           pci 0000:02:00.0: BAR 0: assigned [mem 0xe2000000-0xe3ffffff]
  t=173.7  fpdma 0000:02:00.0: module installed
```

Three things follow.

**The chip appears very late, and by rescan.** 173 seconds into the boot, and
the "type 00 class" / "BAR 0: assigned" pair is the kernel *discovering* a
device on a rescan, not finding one at boot enumeration. Something in userspace
released it and rescanned.

**`scd_finish_init` is not what releases it**, despite sitting 17 seconds
earlier and looking like a candidate. Arista's GPL driver is readable and the
function is interrupt plumbing: it walks the interrupt masks, creates up to 32
UIO devices — one per set bit — and registers the handler. There is no reset
write in it.

**`init_trigger` is a userspace handshake, not a board operation.** The driver's
own comment: after the other attributes are written, writing anything to
`init_trigger` causes initialisation to continue, creating the UIO devices and
registering the handler; afterwards the attribute files become read-only. It
reads `0` on this box, meaning "initialised, no error".

So the release is performed by **EOS userspace, after the SCD driver is fully
initialised** — not by the driver, and not by `NorCalInit`, which runs much
earlier and contains no `resetClear`. That is a narrower target than "something
in EOS", and it is where to look next.

### thorn's initialize writes nothing

Worth recording as a dead end so it is not walked twice. `Thorn.initialize()` is
one line — `self.operatingMode = OperatingMode.Operating` — and the property
setter validates the value and stores it in `self._operatingMode`. **No hardware
write at all.** The interface looked promising (`powerGood`, `powerStatus`,
`powerCycle`, `scdReset`, `clockSelect`, `initValues`) and the initialisation
touches none of it.

### It is a reset PULSE, not a release

The 17-second gap is not empty. `dmesg` in that window:

```
  t=163.4  tg3 ma1: Link is up at 1000 Mbps       (management, unrelated)
  t=173.538907  pcielw 0000:00:04.0:pcie04: link down
  t=173.538917  pcielw 0000:00:04.0:pcie04: link down processing complete
  t=173.642583  pcielw 0000:00:04.0:pcie04: link up
  t=173.642613  pci 0000:02:00.0: [8086:155b] type 00 class 0x020000
```

**A link down and a link up, 104 milliseconds apart.** On a port with nothing
attached there is no link to lose, so that pair is not the chip arriving — it is
the chip being *taken down and brought back*. Something in EOS userspace
toggled the endpoint's reset at t=173, and the link came up on the far side of
it.

That matters because **this port has only ever released.** Every attempt so far
reads the reset register, finds `alta`+`sol`+`rpt` asserted, clears them, and
waits. The vendor's own diagnostics do something different and it has been
visible in the disassembly since the beginning:

```python
reset = scd.hal.resetSet.rd();    reset.rpt=1; reset.alta=1; reset.sol=1;  resetSet.wr(reset)
reset = scd.hal.resetClear.rd();  reset.rpt=1; reset.alta=1; reset.sol=1;  resetClear.wr(reset)
```

Assert, then release. A pulse. It was read as ceremony around a release; the
link-down/link-up pair says it is the point.

**RUN, AND IT DID NOT WORK.** With the reset bits corrected to this board's
`[1, 2, 8]`, the driver asserts all three and releases them in order, and
afterwards `devmem 0xe1004000` reads **`0x00000000`** — every reset released,
by our own code, on a cold board. The chip still does not appear, the root port
still shows no device, and no link event is logged at all.

So the pulse is ruled out along with everything else. What was hypothesised
below is kept because the reasoning was sound and the evidence for it was real;
it simply is not the answer.

What remains unexplained is the specific thing EOS does at t=173 that produces
a link transition. Our sequence produces no link event whatsoever — not a down,
not an up — where EOS logs both 104 ms apart. Something is happening there that
no register this port has read is recording.

**The way to find it is to instrument the vendor's own boot** rather than keep
guessing at it: `spike/scd-dump.c` is already on the switch's flash, so a
script started early in EOS that polls `0x4000` and the root port's `LnkSta`
into a file on flash would catch the transition and whatever precedes it. That
is a direct observation of the one event that matters, and nothing tried so far
has been able to see it.

**The original plan, kept for the record**, was cheap: on a cold board write `0x106`
to `resetSet` (`0xe1004000`), then `0x106` to `resetClear` (`0xe1004010`), and
watch. It is the same class of write already done safely several times, to the
same register.

`pcielw` is also worth noting as a difference in kind: it is an Arista kernel
module watching the root port, and it is what turns a link-up into an
enumerated device. NOSaic had `CONFIG_HOTPLUG_PCI` but not
`CONFIG_HOTPLUG_PCI_PCIE`, so the same event would have produced nothing until
somebody wrote `/sys/bus/pci/rescan`. Now enabled.

### What that suggests is in the way

The SCD has an SPI block of its own, at BAR0 `0x7900`. If the boot flash is
shared between the SCD and the FM6000 — which is the ordinary way to build
this, so that the host can reprogram it — then something has to decide which of
the two masters owns the bus, and on a cold board that has never been told, the
default may not be the FM6000.

That would explain every measurement taken so far: rails on, clock programmed,
resets released, boot ROM unreadable, PCIe never initialised, root port seeing
no device at all.

It also explains why the vendor's boot path contains no `resetClear`. If
releasing the chip is not what starts it, there is nothing for `NorCalInit` to
release.

**Next, in order:** what the SCD's SPI block at `0x7900` is connected to and
whether it arbitrates; whether the board has a separate SPI flash for the Alta
or shares the one the SCD uses; and whether `CHIP_RESET_N` is the SCD's `alta`
bit at all, since the straps are latched on *its* de-assertion and a chip whose
straps were never latched has no boot mode.

### Three hypotheses, all measured, all wrong

### What ruling three things out actually tells us

Taken together the measurements now say something more useful than any of them
did alone:

| | Cold vs warm |
|---|---|
| SCD reset block `0x4000` | warm is `0x000` — **the same state we produced by hand** |
| Si5338 clock, every register read | **identical** |
| thorn `0x23` registers | identical but for reg 5, whose bits the vendor's own class calls fault/clock **status** |

**Everything compared so far is the same on a cold board as on a running one.**
That is a pattern, and it points away from the shape of hypothesis this port
has been testing. "Some device needs programming before the chip will appear"
predicts a difference somewhere, and there is none in the three devices looked
at.

What that leaves, roughly in order of how much they would explain:

- **The CHL8228G has never been found.** It answers at neither `0x70` nor
  `0x30` on any accelerator and bus scanned, so it is somewhere not yet looked
  — and it is the one device in the chain still completely unmeasured.
- **Something that is not a register.** A sequence, a timing, a GPIO, or a
  strap — none of which a cold/warm register diff can see. The `resetSet` →
  work → `resetClear` shape in the diagnostics is a *pulse*, and a pulse leaves
  no trace in either endpoint's register state.
- **Something on the host side of the link.** The RS780 root port at `00:04.0`
  has never been examined; a chip that is powered, clocked and out of reset
  still needs the port to train.

#### The range the first dump stopped before

The same table shows what `0x00`–`0x2f` leaves out. Everything that sets the
output frequency is above it:

```
   0x34 – 0x61   msCtrl[0..3], msCoef[0..3], msnCoef   ← the multisynth config
   0x7b – 0xc4   msFreqInc[0..3]
   0xda          los          0xe6  outputDrive
   0xeb – 0xed   fcal          0xf6  softReset        0xff  page
```

The multisynth coefficients *are* the clock's programming. Reading `0x00`–`0x2f`
and finding it unchanged says almost nothing about whether the clock is
configured — and `page` at `0xff` means there is a second bank we have not
looked at either.

So the position is: **the clock's frequency registers are unread, and the
regulator is unread.** Both live hypotheses are untested, and the one reading
that looked like evidence covered neither.

The next reads are specific: Si5338 `0x30`–`0x70` and `0xda`–`0xf6`, cold and
warm; and the CHL8228G wherever it actually lives, which is not on any
accelerator/bus scanned so far.

### ⚠ The old identification note, kept for the record

Correcting a claim made two sections down before it misleads anyone further.

The board module lists **two** devices at SMBus address `0x70`: the CHL8228G
regulator (`smbusAddress 0x70`) and the **Si5338 clock generator** (also
`0x70`). A scan of the SCD's accelerators, buses 0 and 1, found exactly **one**
responder — `/scd/1/1/0x70` — and it was labelled "the CHL8228G" on the
strength of the address alone.

That was not established. It is one of the two, and the dump below is of
whichever it is.

Attempts to settle it so far:

- `Chl8228G.__init__` sets `mfrModelId = 14` and `deviceIdName = 'CHL8228G'`,
  so the class identifies its part by a model ID — but it reaches the chip
  through a PMBus HAL, and `smbus read8` at a raw offset is not the same
  addressing. No register in the dump reads 14.
- Register `0x02` reads `0x26`, which is 38 decimal, and 38 is suggestive of a
  Si5338 part-number field. Suggestive is not identification.
- Accelerators 0–8 on buses 0–1, and accelerators 0–3 on buses 2–5, were
  scanned for a second `0x70`. **There isn't one.** So the other device is on a
  bus not yet scanned (accelerators 4–8, buses 2–7), behind a mux, or not
  reachable this way at all — and the one responder still has two possible
  identities.

**Why this matters more than a label.** If the device that reads identically
cold and warm is the *clock*, then the clock hypothesis is already weakened by
evidence sitting in this file, and the rails hypothesis is untested rather than
disproven. The two readings of the same dump point in opposite directions, and
which one is right is not yet known.

### The device at `0x70` is identical cold and warm

Read from NOSaic's own CLI on a cold board (`nosaic platform smbus read 1 1
0x70 0x00 48`) and compared against the warm capture above:

**All 48 registers are byte-identical.** Not one differs.

Whichever chip this is, it is in the same state on a board whose ASIC is dark as
on one whose ASIC is forwarding. It comes up configured — presumably from its
own NVM — and **whatever it is, programming it is not the missing step**.

If it is the CHL8228G, the rails hypothesis is disproven and the reasoning that
ran from "prefdl carries the core voltages" through `AltaVoltageRailAdj.py` ends
here. If it is the Si5338, the *clock* hypothesis is the one that just took the
damage, and the rails are simply untested. **See the identification warning
above: this is not yet decided**, and reporting it as settled was wrong.

What it leaves. The vendor's `initialize()` order is still evidence, but the
interesting part of it is no longer `ir`:

```
   scd.initialize → altatemp → ir + 'vidMode' → ucd → hal.resetSet
     → rd / rpt / alta / sol / wr → hal.resetClear → repeaters
```

`ir` looks idempotent on this board. **`ucd` — the UCD90160 sequencer — has not
been read cold**, and the five steps between `resetSet` and `resetClear` have
not been looked at at all. Note also that the vendor **asserts** the resets
before that middle section and releases them after; this port has only ever
released them. Doing work while the chip is held in reset, and only then
letting go, is a different sequence from the one tried.

### The SCD register map for this board

Disassembling `SantaRosaP5.initialize()` and reading `SaguaroHal`'s register
table gives the board's whole SCD map, and it names most of what the earlier
cold/warm diff could only list as offsets. **Addresses are byte offsets into
SCD BAR0.**

| Offset | Register | |
|---|---|---|
| `0x0140` | `scratch2` | |
| `0x0150` | `softError` | |
| `0x0160` | `prickleRev` | prickle's version — `0x002a0000` → 42 |
| `0x0170` | `quillRev` | quill's version — `0x00550000` → 85 |
| `0x01a0` / `0x01b0` | `dnaHi` / `dnaLo` | FPGA device DNA |
| `0x1000` | `writeProtect` | FDL write protect |
| `0x3000`–`0x30b0` | `interrupt0..3` | maskSet / maskClear / status, 3 registers each |
| `0x3100` | `interruptCtrl` | |
| `0x3300` / `0x3400` | `clockMeasureCtrl` / `clock156MeasureResult` | **this is the `0x3400` delta** |
| `0x3800` / `0x3810` / `0x3820` | `altaTimeLo` / `altaTimeHi` / `altaTimeCtrl` | **this is the `0x3800`–`0x381c` delta** |
| `0x4000` | **`resetSet`** | write 1 to a bit to ASSERT that reset |
| `0x4010` | **`resetClear`** | write 1 to a bit to RELEASE it |
| `0x4100` | `SFPTxDisable` | |
| `0x5000` | `powerSupply` | |
| `0x5010`–`0x5340` | `portStatusControl[1..52]` | **per-front-panel-port, stride `0x10`** — 52 of them, one per SFP+ cage |
| `0x5310`–`0x5340` | `qsfpPortStatusControl[49..52]` | the same four ports, QSFP view |
| `0x6000` | `ledFlashRate` | |
| `0x6050` / `0x6060` / `0x6090` | `statusLed` / `fanLed` / `beaconLed` | |
| `0x6070` / `0x6080` | `powerSupplyLed[1..2]` | |
| `0x60d0`–`0x64c0` | `portLinkLed[1..64]` | stride `0x10` |
| `0x7020` / `0x7030` | `usbPower` / `slaveError` | |
| `0x7900` | `spiBlock` | |
| `0x8000` + `0x80`·n | `smbusBlockV2[n]` | the SMBus accelerators — matching what `scdsmbus` already assumes |

`portStatusControl[1..52]` at `0x5010` stride `0x10` is the per-port transceiver
control this port half-knew from "clearing bit 6 of `0x5010` turns a laser on".
It is one register per front-panel port, and the whole block moving between cold
and warm is now explained.

### The reset bits are named, and we had them right

`ResetRegValue` gives the field positions:

```
   alta = bit 1        sol = bit 2        rpt = bit 8
```

A cold `resetSet` reads `0x00000106` — bits 1, 2 and 8 — which is **exactly
`alta` + `sol` + `rpt`**. So the three bits cleared by hand were the right
three, and releasing the FM6000's reset was done correctly.

That closes off a whole line of doubt. Whatever is missing is **not** the reset
bits, and not their polarity, and not a fourth bit nobody found.

### So the gap is upstream of the reset

The disassembly of `initialize()` reads:

```python
frc += self.scd.initialize()
frc += self.altatemp.initialize()
if self.ir:
    frc += self.ir.initialize('vidMode', True)
frc += self.ucd.initialize()
frc += self.scd.initialize()            # a SECOND time

reset = self.scd.hal.resetSet.rd()      # assert
reset.rpt = 1; reset.alta = 1; reset.sol = 1
self.scd.hal.resetSet.wr(reset)

reset = self.scd.hal.resetClear.rd()    # then release
reset.rpt = 1; reset.alta = 1; reset.sol = 1
self.scd.hal.resetClear.wr(reset)
```

Note `rd` and `wr` are the register accessors, not devices — an earlier reading
of the name list here took them for board components and was wrong.

This port has done the last two blocks and **none** of the first five. The
candidates are now specific and short: `scd.initialize()` (called twice, which
is itself a hint), `altatemp.initialize()`, `ir.initialize('vidMode', True)`,
and `ucd.initialize()`. The regulator's registers are identical cold and warm,
so whatever `ir.initialize` does is either invisible in its first 48 registers
or genuinely idempotent here; `ucd.initialize()` has never been looked at.

### ⚠ `DosBoard` is the diagnostic tree, not the boot path

Worth correcting before anyone builds on the sequence above.
`DosBoard` / `DosLib` / `DosComponent` are Arista's **diagnostic** OS modules.
`SantaRosaP5.initialize()` is the diagnostics' board setup, and its register
map and field names are solid facts about the hardware — but it is not what the
switch runs at boot.

Reading the two components it calls confirms they are not the missing step
either:

- **`Saguaro.initialize()`** walks the SMBus accelerators, clears their hams
  and initialises the MDIO accelerators. Nothing to do with powering the Alta.
- **`Ucd90160.initialize()`** calls `_setRail()`, then shows, clears and
  re-clears logged faults. Fault housekeeping, no enable.

### The production path is `NorCalInit`, and it programs a clock

`/usr/bin/NorCalInit` is a 190-byte wrapper around a `NorCalInit` Python
module, and *that* is what `/etc/rc.d/init.d/NorCal` runs at boot. Its `main()`
does, among much else:

```
   identifyCell → verifyAbootCompatibility → readFdlPrefdl / readFdl
   enableScdCrc
   getDmamemSize                      ← the 48 MB the FM6000's DMA works out of
   hasChl822X → updatePowerControllers
   AltaVoltageRailAdj.isRosa → adjustRosaVoltageRails
   UpdateCpld · enableFaultPowerCycle · UpdatePex · enableEgressCreditTimeout
   Si5338 → needsQuartzyConfig → configure      ← PROGRAMS A CLOCK GENERATOR
   configureNet → HwEpochPolicy → PicassoInit
```

Two things stand out.

**There is a clock generator, and the boot path programs it.** `Si5338` is a
Silicon Labs programmable clock; the board module lists it at `0x70` with an
`osc` and a `resetPin`, and `main()` carries the strings *"Programming clock for
sid"*, *"Clock switch failed, using original clock."* and a
`/mnt/flash/skipClockProgram` escape hatch. **A switch ASIC with no reference
clock will not train a PCIe link no matter what its resets say** — which is
exactly the symptom this board has. This is now the strongest remaining
candidate.

**Nothing in `NorCalInit` releases the ASIC's reset.** There is no
`resetClear` in that flow at all. So something else does it — the vendor's
`scd` kernel driver on probe, or EOS's platform agent later — and finding which
is a separate question from finding what makes the chip *ready* to be released.

`updatePowerControllers` and `adjustRosaVoltageRails` are both in the flow and
both named "update"/"adjust": consistent with the cold-versus-warm read showing
the regulator already in its final state and needing nothing.

### ⚠ Reaching it from NOSaic is a licensing question, not just a coding one

The cold half of that read needs NOSaic to talk to an SCD SMBus accelerator,
and **the accelerator protocol is GPL-2.0 in this tree**.
`internal/platformhal/scdsmbus` says why: reading a register *map* — an
address, a bit position — is fact-gathering and no licence attaches, but
transcribing a *protocol* — the request word layout, the transfer sequence,
the reset handling — is a derivative work of Arista's GPL `scd-smbus.c`, and
the licence follows it. That package is the one GPL-2.0 package in NOSaic, kept
separate so the boundary is an import rather than a comment.

So a C reimplementation of the accelerator in `spike/` would be **wrong**: that
directory is Apache-2.0 like the rest of the tree, and a hand-written C port of
the same protocol is the same derivative work wearing a different language.

The right route is the existing Go package, which already implements this and
already carries the right licence. What this board needs is a `platformhal`
entry that uses it — which is the code M1 needs anyway, since programming the
regulator at boot is the platform HAL's job and not a spike's.

### thorn's bits are status, not control

The same introspection settles the register-5 question without writing
anything. The `Thorn` class's methods are **`isPowerPhaseFault`**,
**`clearPowerPhaseFault`** and **`clockSelectStatus`** — fault reporting and
clock status. Bits 7 and 5 of register 5 tracking ASIC power is exactly what a
fault/status register does, and writing them would almost certainly have
achieved nothing.

The experiment below is left documented because the tooling and reasoning are
worth keeping, and because "we thought about poking it and here is why we did
not" is more useful than silence. It is no longer the recommended next step.

### The experiment that settles it, and it has not been run

`spike/thorn-read.c` now has a write path, and it is awkward on purpose: `-w`
refuses unless `-b` names one bus, it says what it is about to write and to
whom, and it reads the value back — because a status bit will not take a value,
and that by itself is the answer.

The sequence, all of it inside a RAM-booted NOSaic on a cold board (the image's
busybox has `devmem`, so the SCD writes need no extra tool):

```
  doas /tmp/thorn-read -r 5 -n 2            baseline: expect 0x01
  doas devmem 0xe1004000 32                 expect 0x00000106, resets held
  doas /tmp/thorn-read -b 1 -r 5 -w 0xa1    THE WRITE
  doas devmem 0xe1004010 32 0x2             release the SCD resets
  doas devmem 0xe1004010 32 0x4
  doas devmem 0xe1004010 32 0x100
  doas sh -c 'echo 1 > /sys/bus/pci/rescan'
  doas /usr/sbin/fm6000-probe               did 02:00.0 appear?
```

Three outcomes, and all three are informative:

- the write does not read back → those are **status** bits, thorn is reporting
  rail state rather than controlling it, and the enable is somewhere else;
- it reads back and the chip still does not enumerate → they are writable and
  not sufficient, and there is more to the sequence;
- it reads back and `02:00.0` appears → **M1 is finished**.

The board recovers from all three by power-cycling, which is the normal way in
and out of this work anyway.

### What the vendor's board initialisation is

`/etc/rc.d/init.d/NorCal` wraps `/usr/bin/NorCalInit`, which logs to
`/var/log/NorCalInit`. On this chassis it identifies the cell, generates an FDL,
and version-checks the four devices above. It shows only *checks*, so it bounds
the problem rather than solving it.

`AltaVoltageRailAdj.py` confirms prefdl's `AltaVdd` sets the core rail, but it
is a **field utility, not the boot path**: it rewrites the prefdl SEEPROM to
raise `AltaVdd` to 1.2 on Rosa boards reading below **1.10**. This chassis reads
**1.01**, so it is one of the boards that tool exists for — worth knowing before
concluding a rail is misprogrammed.

### The old picture, for orientation

```
  power on
     │
     ▼
  SCD comes up          FM6000 is HELD IN RESET by the SCD
     │                  02:00.0 is NOT on the PCI bus
     │                  lspci shows nothing, and nothing says why
     ▼
  "NorCal initialization"   ◀── EOS's name for it, visible in its boot
     │                          This is the step NOSaic has to replace
     ▼
  SCD reset block 0x4000: release
     │
     ▼
  02:00.0 appears          ◀── the kernel already enumerated and found
     │                         nothing, so it must be told to rescan
     ▼
  BAR0 mappable at 0xe2000000
```

A bare kernel that sees no ASIC here is the **expected** state, not a fault.

| Function | PCI | ID | BARs | Notes |
|---|---|---|---|---|
| Dataplane ASIC | `02:00.0` | `8086:155b` | `0xe2000000`, **32 MB**, 64-bit non-prefetchable | Fulcrum `1823:1770`. **Not on the bus until the SCD releases it.** EOS binds `fpdma` to it. |
| Board FPGA (SCD) | `04:00.0` | `3475:0001` | `0xe1000000` **256 KiB**, and `0xe0000000` **16 MB** | Arastra, subsystem NCube `0007`. Word-addressed. Version reg `0x100` = `0x00217361`. EOS binds `scd`. |
| Management NIC | `00:14.6` | `14e4:1699` | — | BCM5785 + BCM50610 PHY on RGMII, `tg3`. |

*(all live, `lspci -v` on the running unit 2026-09-22.)*

A 32 MB BAR0 is 8M 32-bit words, so word addresses run to `0x7FFFFF` — which is
consistent with the block addresses below and is a cheap sanity check on any
address before it is used. **The SCD's second BAR (16 MB at `0xe0000000`) is
undocumented here**; nothing yet says what it is for.

Board names as the vendor uses them: platform **raven**, board **Santa Rosa**,
SCD family **Bodega**, SMBus **Pluto**. They show up in vendor scripts and in
the SID lists, and are worth recognising.

## This box does not reboot

The reset picture above is only half of it. The other half is that there is no
reliable way to restart this chassis from software:

```
   reboot from Linux
        │
        ├─ hardware reset (reboot=p / =t / =h)  ─────▶  HANGS.  every path
        │                                               tried on a bare kernel
        │
        └─ kexec  ─────────────────────────────────▶  what EOS actually does.
                                                       Its halt script tries
                                                       kexec first and calls the
                                                       hardware reset "the old
                                                       way" it falls back to
   recovery that does work:
        PDU  ──▶ apc1 outlet 6         (a human, or a script, but not the box)
        SCD watchdog 0x0120, action 2  ──▶ power cycle    (see below — NOT
                                                            armed at handover)
```

*(live for EOS's behaviour.)* Plan every bring-up iteration around a
four-minute cold cycle, and treat "it will reboot into the other slot" as a
claim to be demonstrated rather than assumed — A/B rollback rests on it.

**The SCD watchdog works, and it is the recovery path to use. live,
2026-09-25.** Demonstrated from a RAM-booted NOSaic:

```
nosaic platform watchdog arm 60000
  armed, 60000 ms. It must be petted before then or the board power-cycles.
nosaic platform watchdog status
  register  0xc1f41770
  armed, 60000 ms, power-cycles on expiry
```

Left unpetted, the board power-cycled and came back on EOS from flash, pingable
110 s after the watchdog fired and with its OSPF adjacency FULL about a minute
later. So a wedged bring-up does **not** need somebody at the PDU: arm the
watchdog before doing anything to the chip and the box recovers itself.

This matters more here than on a board that can reboot, because nothing else
software can reach will restart this chassis.

## Boot chain

```
Aboot 2.1.0-3037058 "norcal2"   (FAT32 /mnt/flash on the USB DOM, /dev/sda1)
   -> boot-config: SWI=flash:/<image>.swi
   -> boot0 (a shell script, ours to write)
   -> kernel + initramfs
```
Same shape as the sibling 7050SX2, and Aboot here likewise boots unsigned SWIs.
*(live.)*

EOS's own kernel command line on this box, for the parts that are the board
speaking rather than EOS preference *(live)*:

```
console=ttyS0 CONSOLESPEED=9600   reboot=p   pcie_ports=native   tsc=reliable
dmamem=48M   platform=raven
```

`dmamem=48M` is the reserved region the FM6000's packet DMA works out of. This
board has **no IOMMU** — the BIOS disables it and the kernel has zero IOMMU
groups — so DMA is to physical addresses and the region has to be reserved on
the command line, as on the Broadcom boards. The address it can sit at depends
on where this board's ~3978 MB of RAM ends; `0x100000000` is past the end here,
the same trap the 7050SX2 documents. **Not yet chosen.**

## The FM6000 register space

Registers are **32-bit words at word addresses** in BAR0, with wide registers
spanning consecutive words — so an address in the tables below is multiplied by
4 to get a BAR0 byte offset. The one exception found so far is the packet DMA
block, which is at BAR0 **byte** offset `0x5000`. Getting that wrong reads a
different block and looks like the chip lying to you. *(derived.)*

```
  BAR0  0xe2000000 ─ 0xe3ffffff        32 MB = 8M words of 32 bits
  ┌──────────────────────────────────────────────────────────┐
  │ byte 0x005000   PACKET DMA   ◀── BYTE offset, not a word  │
  ├──────────────────────────────────────────────────────────┤
  │ word 0x01C000   MGMT    clocks, BOOT_CTRL 0x1C022,        │
  │                         scan chain 0x1C039..0x1C03D,      │
  │                         block clocks 0x1C03A/3B,          │
  │                         sweeper 0x1C048                   │
  │ word 0x01F000   CRM     memory-fill engine (optional)     │
  │ word 0x0E3000   EPL     per-port MAC/PCS                  │
  │                         EPL_CFG_B 0xE3B02 = PCS type      │
  │ word 0x110000   CM      congestion management (largest)   │
  │ word 0x150000   MOD     egress modify  ⚠ off-buses a cold │
  │        ..0x15FFFF       chip if written too early         │
  │ word 0x180000   L2F     dmask table at 0x180000 + 4*idx   │
  │ word 0x200000   STATS      ⎫                              │
  │ word 0x240000   MCAST_MID  ⎬ ECC bank memories — see below│
  │ word 0x260000   MCAST_POST ⎭ ⚠ THESE BITE                 │
  └──────────────────────────────────────────────────────────┘
   word address × 4 = byte offset.  0x7FFFFF is the last word.
```

Blocks that are known to matter, by word address *(derived — from probing the
running chip, not from a datasheet)*:

| Address | Block | What it is |
|---|---|---|
| `0x01C000`–`0x01C0FF` | MGMT | clocks, `BOOT_CTRL 0x1C022`, soft reset, scan-chain `0x1C039`–`0x1C03D`, block clocks `0x1C03A/3B`, sweeper `0x1C048` |
| `0x01F000` | CRM | the memory-fill engine (also a counter rate monitor) |
| `0x0E3000`+ | EPL | per-port MAC/PCS. `EPL_CFG_B 0xE3B02` selects the PCS type; `0x08C0` in `PORT_STATUS` is a port that is up |
| `0x110000` | CM | congestion management — the largest single block in a bring-up |
| `0x150000`–`0x15FFFF` | MOD | modify/egress. Off-buses a cold chip if written before the rest is up |
| `0x180000` | L2F | the dmask table, at `0x180000 + 4*idx` |
| `0x200000` | STATS | repairable bank memory |
| `0x240000` | MCAST_MID | repairable bank memory |
| `0x260000` | MCAST_POST | repairable bank memory |
| BAR0+`0x5000` | packet DMA | TX/RX descriptor rings (byte offset, not word) — the engine itself is **documented**, §7.11 |

### The documented boot sequence

Table 4-1 of 331496-002 ("Reset Process Detail") gives the whole cold boot as
twelve ordered steps. Paraphrased, with the ones that matter to us:

| Step | What |
|---|---|
| 1–3 | reset asserted and released; boot controller transfers the **fusebox** contents to each module; `BOOT_MODE` pins sampled |
| 4 | boot from serial ROM, or stall waiting for the CPU to drive `BOOT_CTRL` — **ours is boot-from-CPU** |
| 5 | write `0xFFFFFFFF` to `SCAN_CHAIN_DATA_IN` to put the core logic and the EPLs into **normal operating mode** |
| 6 | initialise the PLL and wait for lock — up to 80 ms, pollable via `PLL_STATUS` |
| 7 | take EPL, PCIe, MSB and SPICO/SBUS out of reset (`SOFT_RESET` defaults to **all modules held**) |
| 8 | `BOOT_CTRL:Command` = **Initialize FFU Slice Numbers**, poll `BOOT_STATUS:CommandDone` |
| 9 | `BOOT_CTRL:Command` = **Apply Bank Memory Repairs**, poll `CommandDone` |
| 10 | `BOOT_CTRL:Command` = **Initialize All Scheduler Freelists**, poll `CommandDone` |
| 11 | set up the PCIe SerDes and take PCIe out of reset |
| 12 | **initialise memory** — either program the CRM and launch it, *or* "software writes memory manually" |

```
   CHIP_RESET_N released
        │
   1-3  ▼ boot controller runs: fusebox contents ─▶ each module
        │                       BOOT_MODE pins sampled
   4    ▼ boot from serial ROM?  ──no──▶  STALL, waiting for the CPU
        │                                 to drive BOOT_CTRL   ◀── ours
   5    ▼ SCAN_CHAIN_DATA_IN = 0xFFFFFFFF
        │   core logic + EPLs ─▶ normal operating mode
   6    ▼ PLL init, wait for lock            (<=80 ms, poll PLL_STATUS)
        │
   7    ▼ SOFT_RESET: release EPL, PCIe, MSB, SPICO/SBUS
        │   (its default is ALL MODULES HELD)
        │
   8    ▼ BOOT_CTRL:Command = 1  Initialize FFU Slice Numbers
        │   └─ poll BOOT_STATUS:CommandDone
   9    ▼ BOOT_CTRL:Command = 2  Apply Bank Memory Repairs
        │   └─ poll CommandDone
  10    ▼ BOOT_CTRL:Command = 3  Initialize All Scheduler Freelists
        │   └─ poll CommandDone        (4..7 do individual freelists)
        │
  11    ▼ PCIe SerDes up, PCIe out of reset
        │
  12    ▼ initialise memory:  CRM program + launch + wait
        │                     ── OR ── software writes memory manually
        ▼
   ╔══════════════════════════════════════════════════════════════╗
   ║  ONLY NOW is it safe to touch STATS / MCAST_MID / MCAST_POST ║
   ║  Before this, ONE read of an uninitialised word takes the    ║
   ║  chip off the PCIe bus and the host just hangs.              ║
   ╚══════════════════════════════════════════════════════════════╝
```

The `BOOT` command codes are documented too: 1 = initialize FFU slice numbers,
2 = apply bank memory repairs, 3 = initialize all scheduler freelists, and 4–7
initialise individual freelists (array, head storage, TXQ, RXQ).

**This is the spine of M3**, and it is ordered. Two things in it are worth
saying out loud because they cut against how this chip has been approached
before:

- step 12 explicitly allows **software writing the memory manually** instead of
  driving the CRM. The CRM is an option, not a requirement.
- steps 8–10 are **documented boot commands for exactly the initialisation the
  bank memories need**, and they come *after* the scan-chain write and the PLL
  and *before* anything touches a memory.

There is a real tension here with the prior investigation on this chassis, which
concluded that writing `SCAN_CHAIN_DATA_IN` directly is insufficient and that a
several-thousand-iteration per-block scan program is what actually makes memory
writable. The datasheet describes step 5 as a single write. Both can be true —
the vendor may be applying per-block trim from the fusebox on top of the
documented minimum — and finding out which is one of the first experiments
worth running, because the answer decides whether M2 is a day or a month.

**Follow the documented order first, and only go looking for undocumented steps
where it demonstrably falls short.** That is the opposite of how this chip has
been approached so far, and the reason to state it here.

### The bank memories, and why they bite

`STATS`, `MCAST_MID` and `MCAST_POST` are ECC-protected bank memories that come
out of reset **uninitialised**. Reading or writing an uninitialised word raises
an uncorrectable ECC error that the chip escalates to fatal, and the endpoint
**drops off the PCIe bus** — config space and BAR0 both read `0xffffffff`, while
the PCIe link stays up. From the host that presents as MMIO that suddenly takes
forever, or an RCU stall, or a reboot, and never as "your write was rejected".

So they must be ECC-initialised before anything touches them, and the
initialisation itself must not read them. *(derived, and the single most
expensive thing learned about this chip.)*

### How a frame goes through the chip

The FlexPipe pipeline, in the datasheet's own section order (§5.5 to §5.22).
Worth having in front of you, because almost every block in it is a thing this
port has to configure:

```
   wire ─▶ EPL (MAC/PCS) ─▶ ingress
                              │
              §5.5  PARSER ───┤  microcoded. unrolled slices, 4 bytes each.
                              │  out: FIELDS 88 B, FLAGS 40 b, CHECKSUM
                              ▼
              §5.6  MAPPER      SRC_PORT_TABLE, VID, L2/L3 CAM/RAM,
                              │ L4 ports, SCENARIO_FLAGS, FFU action data
                              ▼
              §5.7  FFU         TCAM slices. keys, scenarios, action chains
                              │ ── this is where ACLs live
                              ▼
              §5.8  HASHING ──▶ §5.9 NEXT HOP ──▶ §5.10 L3 ACTION RESOLUTION
                              │
                              ▼
              §5.11 L2 LOOKUP ──▶ §5.12 ALU ──▶ §5.13 POLICERS
                              │
                              ▼
              §5.14 GloRT LOOKUP        the chip's own destination namespace
                              ▼
              §5.15 DESTINATION MASK GENERATION      (L2F dmask table)
                              ▼
              §5.16 EGRESS ACLs ──▶ §5.17 L2 ACTION RESOLUTION
                              ▼
              §5.18 CONGESTION MGMT ──▶ §5.19 REPLICATION ──▶ §5.20 SCHEDULER
                              ▼
              §5.21 EGRESS MODIFICATION   (MOD block — rewrites on the way out)
                              ▼
                            EPL ─▶ wire
                                      and §5.22 STATISTICS off to the side
```
*(documented — the order is the datasheet's own.)*

Two things follow from this picture. The **parser is the only microcoded stage**
— everything downstream is tables and registers, which is why generating parser
microcode is the whole of the firmware problem. And **the CPU is just another
port**: frames to and from the host go through the same pipeline, entering and
leaving at the packet DMA rather than at an EPL.

### The packet DMA engine is documented

§7.11 describes the engine that replaces Arista's proprietary `fpdma`: TX and
RX **buffer-descriptor rings**, power-of-two sized and 32-byte aligned, with a
**16-byte descriptor** — Table 7-5 gives it as Status / Length / Buffer-Addr-Lo
/ Buffer-Addr-Hi — plus scatter-gather and the PCIe and pause behaviour around
it. §3.3.5 covers the DMA interface pins and §8.6 its timing.

```
   BAR0 + 0x5000
   ┌────────────────────────────────────────────────┐
   │ TX ring (power-of-2 entries, 32-byte aligned)  │
   │  ┌──────────────┬──────────────┬────────────┐  │
   │  │ Status       │ Length       │ BufAddrLo  │  │  16 bytes per
   │  │              │              │ BufAddrHi  │  │  descriptor
   │  └──────┬───────┴──────────────┴─────┬──────┘  │  (Table 7-5)
   │         │                            │         │
   │ RX ring │                            ▼         │
   │  ┌──────┴───────┐              host buffer     │
   │  │  ... same    │              (physical addr  │
   │  └──────────────┘               — no IOMMU)    │
   └────────────────────────────────────────────────┘

   order matters on TX:  TX_STOP  (resets the descriptor index)
                            ▼
                         write descriptors READY
                            ▼
                         TX_START
   TX_START on an empty ring puts the processor Idle, and TX_POST
   does NOT wake it.  RX is the mirror image.        (derived)
```

So M5 is implementation from a specification rather than reverse engineering,
which is not true of much else on this chip. *(documented.)*

**Table 7-8 gives the internal frame tag**: the F64/ISL tag, 7 bytes at L2
offset 12, carrying DGLORT, SGLORT, SWPRI, USER and FTYPE.

```
   offset  0        6        12                     19/20
           ┌────────┬────────┬──────────────────────┬──────────────┐
           │  DMAC  │  SMAC  │   F64 / ISL tag      │ ethertype .. │
           │  6 B   │  6 B   │   7 B?  or  8 B?     │   payload    │
           └────────┴────────┴──────────────────────┴──────────────┘
                              DGLORT SGLORT SWPRI USER FTYPE
                              ▲
                              the tag is INLINE in the frame, not in
                              the descriptor's field that looks like it
                              — and the length includes it
```
 Note that the prior
investigation on this chassis recorded it as **8** bytes at that offset when it
snooped a working transmit — `DMAC(6) | SMAC(6) | F64 tag(8) | ethertype`. One
of those is wrong, or the eighth byte is padding to a 4-byte boundary. It is a
cheap thing to settle on the bench and an expensive thing to get wrong, because
a tag off by one byte produces a frame the chip accepts and misparses.

### The SerDes firmware question is not settled

The parser microcode problem is solved below, but it is not the only firmware on
this chip. SerDes bring-up goes through a **SPICO** microcontroller, and whether
its code is a separate vendor firmware file, is embedded in the proprietary
`libFocalpointSDK.so`, or is not needed at all on this part **has not been
established**.

This is the open risk to the claim that images for this board stay publishable.
If SPICO code turns out to be a required vendor blob, then either the ports do
not come up, or the board acquires exactly the non-redistributable dependency
the parser decision avoided. Nothing on this page should be read as saying that
question is answered.

Settling it is M4 work and it should be settled early, because it can invalidate
the licensing shape of the whole port.

### The parser is microcoded, and we write the microcode

This was going to be the licensing problem on this board, and it is not one.

Two different things get called "the microcode" on this chip, and separating
them makes the problem much smaller:

**The parser Action SRAM is true microcode, and it is documented.** The FM6000
parser is an iterative state machine that eats four bytes of frame per
iteration, physically unrolled into one hardware *slice* per iteration. Each
slice has an Action SRAM, and Table 5-3 of the Intel FM5000/FM6000 datasheet
(document **331496-002**, §5.5.5 "Action Encoding") gives its every field:

| Field | Width | What it does |
|---|---|---|
| `StateOp0..3` | 2 each | per state byte: add immediate, load immediate, load frame byte + immediate, or frame byte ×2 + immediate (mod 256) |
| `StateValue0..3` | 8 each | the immediates for the above |
| `StateFrameRot` | 2 | byte rotation applied to this slice's frame data first |
| `SetFlags` | 38 | `FLAGS |= SetFlags` (`IncompleteHeader` and `ParityError` are hardware's) |
| `Halfword0Dest`, `Halfword1Dest` | 6 each | which 16-bit FIELDS output channel each half of the frame data lands in |
| `Halfword0Rot`, `Halfword1Rot` | 2 each | nibble rotation per half — *not available on the top 8 slices* |
| `Byte0..3Enable` | 1 each | per post-rotation byte, whether it overwrites its FIELDS destination |
| `Halfword0Add`, `Halfword1Add` | 1 each | add this half to `CHECKSUM` (ones-complement; drives the `ChecksumError` flag) |
| `ShiftNextSlice` | 3 | advance the next slice's parsing window by 0..7 bytes, mod 8 |
| `LegalPadding` | 2 | bytes of the four this slice requires; failing it sets `IncompleteHeader` (bit 37) |

```
   frame bytes ──▶ │ 4 B │ 4 B │ 4 B │ 4 B │ 4 B │ ...
                     │     │     │     │     │
                     ▼     ▼     ▼     ▼     ▼
                  ┌─────┬─────┬─────┬─────┬─────┐
   STATE 32 b ───▶│ sl0 │ sl1 │ sl2 │ sl3 │ ... │──▶ (threaded slice to slice,
                  └──┬──┴──┬──┴──┬──┴──┬──┴──┬──┘     plus a 3-bit window shift)
                     │     │     │     │     │
     each slice:     ▼     ▼     ▼     ▼     ▼
     ┌──────────────────────────────────────────────┐
     │ Action SRAM entry  (~107 documented bits)     │
     │  StateOp0..3 / StateValue0..3  StateFrameRot  │
     │  SetFlags 38b                                 │
     │  Halfword{0,1}Dest 6b  Rot 2b  Byte{0..3}En   │
     │  Halfword{0,1}Add   ShiftNextSlice  LegalPad  │
     └───────────────┬───────────────────────────────┘
                     ▼
        FIELDS 88 B  ·  FLAGS 40 b  ·  CHECKSUM 16 b
                     │
                     ▼   Table 5-5 fixes which FIELDS channels the
              downstream pipeline reads; §5.5.9 fixes which FLAGS
              the fixed-function logic acts on. That pair is the
              contract our microcode must meet. Everything else
              about the parse is ours to choose.
```

The pipeline is **unrolled**: there is no branch instruction and no program
counter, so a parser "program" is a table with one action per slice, and all
conditional behaviour is carried by `STATE` and the window shift.

That is about 107 bits of documented fields per slice, against the roughly 128
bits the block diagram shows. There are no branches to implement: the pipeline
is unrolled, so a parser "program" is a **table** — one action per slice — and
control flow is carried by the 32-bit `STATE` vector the slices thread through
and by the window shift.

Outputs are an 88-byte `FIELDS` bus, a 40-bit `FLAGS` vector and a 16-bit
checksum. Table 5-5 fixes which FIELDS channels the downstream pipeline reads,
and §5.5.9 says which ACTION_FLAGS bits the fixed-function logic downstream acts
on — so those two tables are the contract our microcode has to meet, and
everything else about the parse is ours to choose.

**The FFU and mapper are not microcode.** They are CAM and SRAM contents —
scenario keys, action chains, action data (§5.6, §5.7) — which is to say they
are *configuration*, generated from a port map and a rule set the way every
other board's forwarding tables are. Calling them microcode makes the job sound
like something it is not.

So: **no vendor parser microcode.** NOSaic writes a generator and emits the
Action SRAM itself. That settles the parser and it does not settle the SerDes —
see the SPICO question above. What it parses is then our decision rather
than Arista's — and a switch that needs Ethernet, VLAN, IPv4, IPv6 and TCP/UDP
needs a good deal less parser than a vendor image that supports everything.
Arista's own blobs make the point: their standard pipeline, a PDP variant and a
tap-aggregation build are three *different programs* for the same silicon.

The generator is Apache-2.0 code in this repository like anything else.
Knowledge taken from a datasheet is not vendor code: the datasheet itself is
Intel's and is fetched rather than committed, the same rule
[docs/datasheets.md](../../../docs/datasheets.md) applies to every other board.

What is not yet known, and has to come off the chip: **where the Action SRAM
lives** in the register map, how many slices this part has, and how a slice's
SRAM is indexed by state. The encoding is documented; its address is not.

## Front panel

### The port map, recovered from the vendor agent

`/var/log/agents/FocalPointV2-3350` prints its whole mapping at bring-up, one
line per physical port:

```
SwitchPostInitialize port 40 --> logPort 1
SwitchPostInitialize port 20 --> logPort 2
...
```

75 physical ports, of which 54 carry a logical port and 21 are unused. **live**

| front panel → physical | | | |
|---|---|---|---|
| 1 → 40 | 2 → 20 | 3 → 41 | 4 → 21 |
| 5 → 42 | 6 → 22 | 7 → 43 | 8 → 23 |
| 9 → 36 | 10 → 64 | 11 → 37 | 12 → 65 |
| 13 → 38 | 14 → 66 | 15 → 39 | 16 → 67 |
| 17 → 72 | 18 → 28 | 19 → 73 | 20 → 29 |
| 21 → 74 | 22 → 30 | 23 → 75 | 24 → 31 |
| 25 → 68 | 26 → 24 | 27 → 69 | 28 → 25 |
| 29 → 70 | 30 → 26 | 31 → 71 | 32 → 27 |
| 33 → 32 | 34 → 60 | 35 → 33 | 36 → 61 |
| 37 → 34 | 38 → 62 | 39 → 35 | 40 → 63 |
| 41 → 52 | 42 → 56 | 43 → 53 | 44 → 57 |
| 45 → 54 | 46 → 58 | 47 → 55 | 48 → 59 |
| 49 → 44 | 50 → 45 | 51 → 46 | 52 → 47 |

It is not a tidy mapping and there is no arithmetic that generates it — odd
front-panel ports come off one set of physical ports and even off another, in
blocks of four, which is what a four-lane EPL feeding two rows of cages looks
like once the board routing is taken into account.

Physical ports with no logical port: 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 48, 49, 50, 51. **live**

Logical 53 → physical 3 and logical 54 → physical 1 sit outside the 52 SFP+
cages. They are almost certainly the CPU-side ports, but nothing here confirms
that. **derived**

The mapping from physical port to *EPL instance and lane* is still not
established — this table stops at the physical port number the vendor's agent
uses. **UNKNOWN**


```
   ┌────────────────────────────────────────────────────── ARISTA 7150S-52 ──┐
   │  1  3  5  7 ...                                             ... 49  51  │
   │ ┌─┐┌─┐┌─┐┌─┐                                                 ┌─┐┌─┐     │
   │ └─┘└─┘└─┘└─┘   52 × SFP+  10G   (top row odd, bottom even)   └─┘└─┘     │
   │ ┌─┐┌─┐┌─┐┌─┐                                                 ┌─┐┌─┐     │
   │  2  4  6  8 ...                                             ... 50  52  │
   └──────────────────────────────────────────────────────────────────────────┘
        no QSFP · no external PHYs · every port direct serdes off the FM6000

   front panel N  ─────▶  physical port  ─────▶  serdes lane
                          (see the table below)
```

52 SFP+ cages, 10G, numbered 1..52 on the silkscreen. No QSFP, and no external
PHYs — every port is direct serdes off the FM6000, so there is none of the PHY
firmware loading the 7050TX-64 and AS4610 need. *(The odd-above-even panel
layout is the usual Arista arrangement and is `assumed` until someone looks at
the box.)*

**The port map does not exist yet.** Front-panel number to EPL instance to
serdes lane is the first table this board needs and the first thing the datapath
will be wrong about. `platform/*/tools/mkportmap.sh` on the Broadcom boards
builds theirs from a `config.bcm`; there is no equivalent file here, so this one
is measured — light one cage at a time and see which EPL's `PORT_STATUS`
changes.

## The SCD, and the watchdog that is this board's only real recovery

All of the board's own hardware is behind the SCD: LEDs from `0x5010` (clearing
bit 6 turns an SFP laser on), the switch reset block at `0x4000`, interrupts at
`0x3000`/`0x3030`/`0x3060`. The cage EEPROM and sensor layout for *this* board —
which accelerator, which bus, which address — is not known, and the sibling
boards' layout is not transferable. *(derived for the LED and reset regions;
unknown for the rest.)*

`internal/platformhal/scd` already drives this FPGA family for the sibling
Arista boards, and its layout is architecturally fixed across Arista platforms:
the switch reset block is at `0x4000` everywhere from Trident2 to Tomahawk4, and
**the watchdog is at `0x0120`** — a register that file already documents as the
one this board uses. Only the bit assignments vary by platform, and this board's
have not been checked.

That watchdog matters more here than on any other board in the tree. Bit 31
enables it; bits `[30:29]` select the action, and **2 is a power cycle** rather
than a warm reset. On a board whose hardware reset hangs and whose vendor OS
reboots by `kexec`, a watchdog that power-cycles is the only recovery path that
does not involve a human at the PDU — and it is the difference between an A/B
rollback that can work here and one that cannot.

**It is not armed when a NOS starts.** Aboot punches it during boot and hands
over with it disarmed, so an image begins with no recovery net at all. The
7050SX2 records learning that the expensive way.

## NOSaic booted on this switch, 2026-09-23

RAM-booted through Aboot over HTTP. Nothing written to the board; a power cycle
returned it to EOS.

```
NOSaic: staged, not booting (testonly)        <- the dry run first, as designed
Linux version 6.12.105 (nosaic@nosaic) ... crosstool-NG 1.28.0
MPTABLE: OEM ID: Arista   Product ID: mainboard
NOSAIC-INITRAMFS booting from RAM, no partitions used
NOSAIC-INITRAMFS overlay assembled (persistent=no)
NOSAIC-S6 compile rc=0 / scan directory live after 1s / init rc=0
nosd: output to /var/log/nosd/current
nosaic login:
```

**M0's build-and-boot half is done.** Three things it exposed:

### 0. The management NIC does not come up

```
tg3 0000:00:14.6: No PHY devices
tg3 0000:00:14.6: Problem fetching invariants of chip, aborting
NOSAIC-NET route default via 10.10.33.1 FAILED
NOSAIC-NET waiting for: eth0
```

Not the MAC problem the sibling 7050SX2 has — that one is already patched and
this board gets past it. This is **PHY discovery**: the BCM50610 in front of the
BCM5785 never answers on the MDIO bus, so tg3 gives up before there is an
interface at all.

Two causes, and both were real:

- **`CONFIG_BROADCOM_PHY` was not set on x86_64.** It is in the armhf fragment
  and was never in this one, so `drivers/net/phy/broadcom.c` was not built and
  the BCM50610 had no driver on this architecture at all.
- **The kernel actively disables the PHY's internal RGMII clock delays.** This
  board needs the PHY to supply *both* the RX and TX internal delays. Older
  kernels left the PHY's power-on defaults alone; since the delays became
  explicit, `bcm54xx_config_clock_delay()` turns both **off** for plain
  `PHY_INTERFACE_MODE_RGMII`, which is the only mode tg3 will accept —
  `tg3_phy_init()` returns `-EINVAL` for anything else, so simply asking for
  `RGMII_ID` is not available.

Fixed by `recipes/linux/patches/0003-...`: a `PHY_BRCM_FORCE_RGMII_DELAYS`
dev_flag that means "supply both internal delays whatever the interface mode
says", honoured where the skews are enabled and set by tg3 for the BCM50610.
Additive, so it only ever turns a delay on and no board that works today
changes behaviour.

**Confirmed on hardware 2026-09-24.** With both changes in:

```
Broadcom BCM50610 a6:01: attached PHY driver (mii_bus:phy_addr=a6:01, irq=POLL)
tg3 0000:00:14.6 eth0: Tigon3 [partno(none) rev 5785041] (PCI Express)
tg3 0000:00:14.6 eth0: Link is up at 1000 Mbps, full duplex
```

The PHY attaches at `a6:01`, `eth0` exists, and the link comes up at a gigabit.
The MAC is still the random fallback from patch `0001` — "only a default address
available; using a random one until the board supplies its own" — which is the
existing stopgap doing its job until the prefdl reader exists.

### 1. The login works — I used the wrong account

Recorded because the wrong version of this was written down first, and a note
saying a board cannot be logged into is exactly what nobody re-checks. `root` is
**deliberately locked** (`root:*` in the image's shadow file); the account is
**`admin`**, no password, console only, per `base/identity.yml`. What the board
*was* missing is a `config/` directory, so it came up with no management
address. It has a `network.conf` now.

### 2b. declaring `platform_hal` made the board boot to a rescue shell

Found the hard way, on the boot after adding `platform_hal:` to `board.yml`:

```
nosaic: context deadline exceeded
s6-rc: warning: unable to start service asic-release: command exited 1
NOSAIC-S6-FAIL the service database did not come up
NOSAIC-RESCUE shell on the console; the system is NOT running
```

Declaring a platform HAL adds an `asic-release` oneshot. On this board the
release genuinely fails — the chip does not come onto the bus, which is the
entire M1 problem — and **an s6 oneshot that exits non-zero takes the whole
service database with it.** The switch came up with no network, no login and no
services: strictly worse than a switch that boots and reports a dead datapath,
and precisely the state a board under bring-up is in every time.

Fixed by running the release through a script that reports a failure instead of
being one. **This does not weaken A/B rollback**, which is the obvious
objection: the signal that an image is broken is `nosd` exiting non-zero when
it cannot find its chip, and trial-confirm acts on that. The oneshot failing
was a second, redundant copy of the same signal — and the redundant copy is the
one that costs an operator their console.

The same file already warned about this shape for the C CLI: *"a generated
service that runs a refusal exits non-zero and takes the service database down
with it"*. The warning was right and the case it guarded against was too
narrow.

### 2a. `thermal` restart-loops too, for the same reason

Declaring `platform_hal` in `board.yml` started the thermal service, and it
immediately did what `nosd` used to: respawn several times a second, printing
`thermal: output to /var/log/thermal/current` each time. 98 lines of it in one
short session, and it swallowed the output of the command being run at the
time.

Same shape as the `nosd` bug and the same cause: a service that cannot do its
job exits, `restart: always` brings it straight back, and on a board where the
thing it needs is *legitimately* absent that is an infinite loop rather than a
fault. Here the board has no `smbus:` sensor map — deliberately, because this
board's is not known and the sibling's is not transferable — so there is
nothing for the thermal loop to read.

Fixing it properly is the same fix: a service with nothing to work with should
back off, not spin. Fixing it accidentally by inventing a sensor map would be
worse than the bug.

### 2. `nosd` restart-looped and flooded the console — fixed

It exited non-zero the moment it could not find the chip, and the service is
`restart: always`. On a board whose ASIC is legitimately absent until something
releases it, those combined into a respawn several times a second that made the
console unusable. It now **waits up to 30s for the chip and then exits
non-zero**: exiting keeps the A/B semantics, waiting stops it racing the
platform HAL, and the wait is what bounds the restart rate.

### 3. `boot --testonly` left the management interface down — fixed

`boot0` cycles `ma1` up then down before the kexec — the up makes Aboot's `tg3`
copy the MAC out of the SCD mailbox, the down stops the NIC DMA-ing into the
next kernel. Both right before a jump, but they ran **before** the `testonly`
exit, so a dry run left the interface down and the next `boot http://...` in the
same session failed with `Network is unreachable`. A dry run must leave the
board as it found it.

## How NOSaic fits on top, and what has to be written

Nothing here runs yet. This is the shape it has to take, drawn beside a working
Broadcom board so the difference is visible rather than implied:

```
        every other board                      arista-7150s-52
        ────────────────                       ───────────────

   nosaic CLI                             nosaic CLI
       │ newline-delimited JSON               │  ── SAME SOCKET, SAME
       ▼ /run/nosd.sock                       ▼     PROTOCOL, unchanged
   ┌──────────────────┐                   ┌──────────────────┐
   │ nosd-td2 / -td2p │                   │  nosd-fm6000     │
   │ -tdp / -helix4   │                   │                  │
   ├──────────────────┤                   ├──────────────────┤
   │ openbcm SDK      │  ◀── 874 MB of    │  (nothing here)  │
   │ bcm_* / soc_*    │      vendor code  │                  │
   ├──────────────────┤                   ├──────────────────┤
   │ userspace BDE    │                   │ our register code│
   │ over mmap(BAR0)  │                   │ over mmap(BAR0)  │
   └────────┬─────────┘                   └────────┬─────────┘
            │ CMIC                                 │ no CMIC
            ▼                                      ▼
        Broadcom ASIC                          FM6000

   Linux side is identical on both:
        taps swp1..swpN ◀──▶ tapbridge poller ◀──▶ chip CPU port
                │
                ▼
        Linux IP stack ──▶ FRR (OSPFv2 / OSPFv3)
```

**The socket is the contract**, and it does not change: the CLI, the config
model and the HAL above it never learn which silicon answered.

What that costs, concretely — of the shared code in `datapath/common/`, only
some is chip-agnostic:

| File | Reusable here? |
|---|---|
| `mmio.h`, `props.c`, `portmode.c` | **yes** — no chip calls in them |
| `dmapool.c` | probably, it is a physical-memory allocator |
| `tapbridge.c` | **no** — built on `bcm_tx` / `bcm_rx` |
| `query.c` | **no** — answers from `bcm_port_*`, `bcm_vlan_*`, `bcm_l3_*` |
| `l3sync.c`, `acl.c` | **no** — same reason |

So `datapath/fm6000/` needs its own packet path, its own socket server, its own
FIB mirror and its own ACL programming. They must speak the **same JSON** as the
Broadcom ones, which is what `internal/nosd/proto` and
`internal/switchapi` define — those are the specification, not `query.c`.

The build is simpler than any other board's, though: no SDK to stage, no vendor
tree to fetch, `-lpthread` and libc.

## First contact: NOSaic's own code read this chip, 2026-09-23

`fm6000-probe`, built static and run under EOS on the warm lab board, against a
chip EOS had configured and was actively forwarding on. Read-only.

```
chip     0000:02:00.0  (8086:155b)
BAR0     33554432 bytes mapped (32 MB), words 0..0x7fffff
on bus   yes

  BOOT_CTRL            0x01c022 = 0x00000313     predicted warm 0x313   ✓
  SCAN_CONFIG_DATA_IN  0x01c03a = 0xffffffff                            ✓
  SCAN_CHAIN_DATA_IN   0x01c03b = 0xffffffff                            ✓
  SWEEPER              0x01c048 = 0x0008bb2c     predicted warm         ✓
  EPL_CFG_B            0x0e3b02 = 0x00090003     10GBASE-R              ✓
```

Five predicted values in a row. That confirms more than five addresses:

- **the word addressing is right.** A wrong stride would not have produced five
  correct values; it would have produced five plausible wrong ones.
- **`pci.c` works on real silicon**, including the sysfs `resource0` mapping —
  which works even with the vendor's `fpdma` driver bound to the device, so
  looking at this chip does not require unbinding anything.
- **BAR0 is exactly 33554432 bytes**, so the 8M-word address space and the
  `0x7fffff` ceiling are measured rather than inferred.

### The warm MGMT fingerprint, and what it identified

A dump of `0x1c000`–`0x1c07f` gave the first complete picture of the control
block on a working chip. Two new identifications came straight out of it:

| Word | Warm value | |
|---|---|---|
| `0x1c021` | `0x00000208` | **PIN_STRAP** — the prior work's cold bring-up starts from "PIN_STRAP=0x208". A value matching a documented value is strong evidence, not proof |
| `0x1c038` | `0x0101e848` | **bit 24 set**, exactly as the warm-versus-cold delta predicted. One of the few known handles on "has this chip been brought up" |

Other non-zero words in the block, unidentified and recorded so the cold diff
has something to subtract from: `0x1c001` `6ffe`, `0x1c002` `3fff`,
`0x1c003` `7fff`, `0x1c01d` `ffffffff`, `0x1c01e` `fffc0000`,
`0x1c01f` `0009502f`, `0x1c025` `278`, `0x1c026` `380278`, `0x1c027` `ffffffff`,
`0x1c028` `02900c81`, `0x1c030` `03c00000`, `0x1c031` `3010`, `0x1c033`
`20000000`, `0x1c037` `3e`, `0x1c03d` `188`, `0x1c042` `20841438`,
`0x1c043` `5560`, `0x1c044` `08011b05`, `0x1c046` `7`, `0x1c049` `2`,
`0x1c04b` `0030a2c3`, `0x1c04c` `2000`, `0x1c050` `10`.

### The experiment this sets up

`SOFT_RESET` reads `0x16` cold and `0` warm. `PLL_STATUS` and `BOOT_STATUS`
likewise differ between the two states. Warm alone cannot pick them out — most
of the block is zero — but **a cold dump of the same range, diffed against the
warm one above, should leave very few candidates, and only one holding exactly
`0x16`**.

That one experiment unblocks Table 4-1 steps 6 through 10, which is most of
`boot.c` and includes step 9, "apply bank memory repairs". It needs the board
cold: power cycle, and read the block before anything configures the chip.

## What NOSaic actually drives today

`datapath/fm6000` exists and builds. It does **not** forward, bring ports up or
program anything — and it reports every capability as false, so `nosaic show
caps` on this board tells the truth rather than a plan.

What is real:

| | |
|---|---|
| `pci.c` | finds `8086:155b`, maps BAR0 through **sysfs `resource0`** rather than `/dev/mem` — so unlike the Broadcom boards this needs no `iomem=relaxed`, and the mapping is bounded to the device's own BAR |
| | word- and byte-addressed accessors, **off-bus detection**, and a guard that refuses the ECC bank memories and the ESCHED read hazard until something says they are safe |
| `boot.c` | Table 4-1 as twelve named steps. Runs steps 1–5, waits the documented PLL time, and then **refuses by name** because `PLL_STATUS`, `SOFT_RESET` and `BOOT_STATUS` addresses are not established |
| `sock.c` | the switch-api socket, same JSON as every other datapath, plus `asic.state` and `asic.reg` |
| `probe.c` | `fm6000-probe` — a separate binary, because when this chip misbehaves the daemon is usually the thing that is wrong |

### The off-bus detector, and why it is built the way it is

All-ones is both the signature of a departed chip and a legitimate register
value, so the BAR alone cannot tell them apart. The accessors treat `0xffffffff`
as a *suspicion* and confirm it against **PCI config space**, which is the only
place the two cases differ — a live endpoint answers its vendor ID and a fatal
one answers `0xffff` there too.

Writes get the same check, and that matters more: a write cannot report failure,
and a write is precisely what kills this chip. The check costs a sysfs read, so
bulk writers — the memory fill is over a million words — turn it off with
`fm_set_write_check()` and **owe a check when the burst ends**. That trade gives
up knowing *which* write did it, which for a uniform fill is not information
anyone wanted.

### The guard is not politeness

## What is actually on the board's SMBus, 2026-09-27

The note in `board.yml` used to say a scan of `0x58`..`0x68` on accelerators 0
and 1 found no fan controller. That scan was too narrow and, more
importantly, too trusting: **this bus times out often enough that a part
which is definitely there fails individual reads**, repeatably — dumping 44
registers from one device returns a mix of values and timeouts every time. A
single-shot probe reports present parts as missing.

`nosaic platform smbus scan` now sweeps `0x08`..`0x77` on every bus of both
accelerators, and it needs two rules to say anything true:

- **an all-ones read is silence, not a device.** The master reports success
  whether or not anything acknowledged, and an unpulled line reads `0xff`.
  Without this the scan reports 1120 "devices" on this board.
- **retry before calling an address silent.** Three tries. Without this the
  scan's own flakiness is indistinguishable from absence — the same trap that
  produced the original "no fan controller" conclusion.

Sixteen parts answer: **live**

| where | address | what |
|---|---|---|
| accel 0 bus 0 | `0x4c` | LM90-compatible, mfr `0x01` dev `0x11` — **declared** |
| accel 0 bus 0 | `0x50` | reg0 `0x0d`, unidentified |
| accel 0 bus 1 | `0x0c` | SMBus Alert Response — returns `0x9c` = `0x4e << 1` |
| accel 0 bus 1 | `0x48` | in the LM75 range and **not a sensor** — see below |
| accel 0 bus 1 | `0x4e` | sparse register map, and the part asserting SMBALERT# |
| accel 0 bus 2 | `0x40` | reg0 `0x00`, unidentified |
| accel 0 bus 2 | `0x4c` | **a second LM90**, mfr `0x01` dev `0x11` — **declared** |
| accel 0 bus 3 | `0x30` | reg0 `0x23`, unidentified |
| accel 0 bus 4 | `0x50`+`0x58` | a PSU: FRU EEPROM and its controller |
| accel 0 bus 5 | `0x0c`, `0x4e` | the alert pair again |
| accel 0 bus 6 | `0x73` | reg0 `0x00`, unidentified |
| accel 1 bus 0 | `0x50`+`0x58` | the second PSU |
| accel 1 bus 1 | `0x70` | reg0 `0x09`, unidentified |

Two things fall out of that beyond the fan search.

**A second thermal sensor, and it sees the hot part.** The part at accel 0
bus 2 answers the same manufacturer and device IDs as the declared one, and
both its diodes read sensibly — local 30–31 °C, remote 33–34 °C. Its remote
diode is the hottest thing measured on this board and the only one that reads
hotter than its own local diode, which is what a sensor on a switch die looks
like and is the opposite of the first part's remote. It is declared as
`board2`/`remote2`, named for where it is rather than for what it might be
watching. The cooling loop tracks the hottest sensor, and this board was
running one that could not see the hottest place on it:

```
temp board    29.0 °C      temp board2   31.0 °C
temp remote   26.0 °C      temp remote2  33.0 °C
```

**The part at `0x48` looks like a third sensor and is not one.** It is in the
LM75 address range and its register 0 reads `0x11`, a believable 17 °C. But
an LM75's four registers wrap every four addresses and this part's do not —
`0x00`–`0x0f` read `11 40 7f 80 09 09 09 01 01 01 01 01 01 01 01 01` — its
hysteresis and overtemperature registers would be +127 °C and −128 °C, and
register 0 does not move while the two real sensors drift. It is left
undeclared. A stuck sensor is worse than a missing one: it does not fail, it
under-reports for ever, and the cooling loop believes it.

**The `0x50`+`0x58` pairs are the power supplies**, not the prefdl SEEPROMs an
earlier note guessed at — the real prefdl is on host i2c-1 at `0x52` and has
been read for months.

### The fan controller is still not found

The pair at `0x4e` was the best candidate and does not fit. Read against the
crow CPLD map its "fans present" register is `0x00` and its tachometers read
zero while a fan ID register is set, which is not a fan controller with fans
on it. Its stable registers are `0x00`–`0x02`, `0x19`, `0x20`, `0x21`, `0x25`,
`0x26`, `0x2a`.

⚠ **Do not go looking by writing.** Two earlier attempts to find a fan CPLD's
PWM register by sweeping it powered a switch off. Everything above is reads
only, and the scan has no write path.

Nor is it on an accelerator nobody looked at. The cages occupy accelerators
2–8; **accelerator 9 exists and holds none of them**, which made it the
obvious place left — and it is empty: 0 devices, 560 addresses reading
all-ones, 336 never answering.

And it is not in the board controller either. `nosaic platform scd diff`
reads a register range three times a second apart and reports what moved,
because **a tachometer counts and a configuration register does not** — which
finds the registers that measure something without knowing their names and
without writing anything. Over the whole 64 KB BAR exactly four words move,
`0x3810`–`0x381c`, and they are four aliases of one free-running counter:
all four read the same value and it advances by about 3.6 × 10⁸ per second,
so a counter clocked near 362 MHz. Four fans would be four different small
numbers. There are no tachometers in this controller.

None of this is stale state left by the vendor OS either. After the mains
were pulled on 2026-09-27 the picture is identical: still no `0x60` on any
bus of either accelerator, and `0x4e` is still pulling SMBALERT# on buses 1
and 5. The alert is a standing hardware condition, not something left over.

So the search is now bounded on three sides — both board accelerators, the
spare accelerator, and the controller's own registers — and the remaining
possibilities are that the part is held in reset by a step NOSaic does not
perform, or that this chassis regulates its fans in hardware and offers the
OS nothing to drive. Neither is resolvable from here without the vendor OS or
documentation.

No controller is declared, no cooling loop starts, and this board reports
temperature and regulates nothing. That is now a bounded gap with an
inventory behind it rather than a shrug.

⚠ Everything above is reads only, and `scd diff` has no write path by
design. Two earlier attempts to find a fan PWM register by sweeping a CPLD
powered a switch off.

## Cage presence: the decode, and how it was settled, 2026-09-26

The driver used to match the cage word as a whole against three values
sampled while the vendor OS was driving the board controller. NOSaic leaves
that controller in a different state, so on this switch **every one of the 52
cages read a word the driver had no meaning for** and the whole table came
back `undetermined` — including the four cages that had modules in them.

The comment where it matched whole words was right to be careful: an earlier
version guessed a bit decode from three samples and reported every cage as
populated. What settles it is ground truth rather than more samples. **A
module's EEPROM answers on the cage's own SMBus channel whether or not the
board controller's word can be read**, so presence is establishable per cage
without reference to this register at all. All 52 cages were probed that way
and correlated:

| | module in cage | EEPROM answers | word |
|---|---|---|---|
| cages 1–4 | yes | identifier `0x03`, SFP | `0x00000180` |
| cages 5–52 | no | `0xff` | `0x00000187` |

No exceptions, and bits 1–2 then agree across all five whole words ever
measured on this board, under both operating systems: set on every empty
cage, clear on every populated one. Bit 6 is the documented laser gate. So
presence is `word & 0x6 == 0` and the laser is `word & 0x40 == 0`, checked
against an independent signal on every cage rather than inferred from a
handful of readings.

### And the cages do come up powered, 2026-09-27

The open worry was that only the vendor OS configures the cages, so a switch
that had never run it would show a module as an empty cage. This switch could
not answer the question — its SCD had been configured by EOS and a NOSaic
boot does not reset it — so the mains were pulled at the PDU. The outlet was
confirmed by the name the PDU itself prints against it, and the command was
confirmed to have reached *this* box by a down-then-up transition on its
management address, which is the check the lab's own power notes insist on
after three rounds of diagnosis were once built on a PDU command that
switched the wrong outlet.

It came back with every cage configured: **live**

```
4 populated, 48 empty, 0 not powered, 0 undetermined, of 52 cages
```

cages 1–4 at `0x180`, 5–52 at `0x187`, and the module EEPROMs agreeing
exactly — 1–4 answer with identifier `0x03`, 5 and 6 read `0xff`.
`release-asic` does not disturb them either. **Optics work from a cold start
with no vendor OS involved, and NOSaic has nothing to do here.**

The `0x1DF` reading was real when it was recorded and is not reproducible on
current software, so what produced it is genuinely unknown rather than
explained away. The decode keeps handling it: reporting `cage not powered`
instead of `empty` costs nothing and stays correct if that state returns.

⚠ **`0x1DF` is still matched as a whole word, and must be.** It is a cage
nothing has powered, and a module in one is *invisible* — no EEPROM, no
presence bit. The bit decode would call it empty, which is the silent
under-report this whole story turned on. There is exactly one sample of it,
so working out which bit means "unpowered" from that would be the original
mistake again. It now reports as `cage not powered`, and the CLI says plainly
that this is a step NOSaic does not yet perform rather than something it
cannot read.

`fm_rd`/`fm_wr` refuse the bank ranges and ESCHED `0x2000` outright until
`fm_bank_mark_initialised()` has been called, and only the code that genuinely
initialises them is entitled to call it. The instinct when a chip misbehaves is
to go and read more registers; on this part that is what kills it.

## The egress scheduler, and what the `0x2000` read hazard actually was

*2026-09-26.* For most of this port the notes carried a rule that read
"ESCHED `0x2000`: reading this off-buses a cold chip". That rule was true,
and it was wrong in all three of its particulars: it named one word of an
8192-word block, it blamed reads when writes are just as fatal, and it called
the chip cold when the chip had completed the documented boot.

**What is actually true.** On a chip that has been through Table 4-1, with
`BOOT_CTRL` at `0x313` and every other block in the low register space
answering normally:

| | |
|---|---|
| a single **read** of `0x2020`, `0x2080` or `0x3800` | chip off the bus immediately |
| **writes** to any of them | about nineteen succeed; the twentieth wedges it |
| the same writes spaced a third of a second apart | identical — the twentieth |
| the same writes after the memory BIST | the **third** |
| a block survey of `0x0000`–`0x1ffff`, one word per 4096 | `0x2000` and `0x3000` are the only fatal blocks; everything else, `0x1a000` included, answers |

The write count is not a timing artefact — microseconds and hundreds of
milliseconds give the same number — and it is not a global budget, because
`--saf` writes 168 registers through the same path without trouble.

**The mechanism.** A read has to complete and a posted write does not. If the
block cannot complete an access at all, a read hangs the local bus at once,
while writes queue in the SCD's bridge until the queue is full and the one
that finds it full blocks forever. Nineteen is the queue. Running the BIST
first spends most of it elsewhere, which is why the budget falls to three.

**The cause.** The egress scheduler is clocked off the scheduler ring, and the
ring is not circulating. This is not a new discovery so much as one we had
already made and filed away: our own prior-art probe on this chassis was
written specifically to test it, and its header says in as many words that
`FOUND` means "pursue ESCHED bring-up" and `NOT FOUND` is "the real wall".

**Where it stands.** `ssched.c` implements the ring init — tick, sweeper, the
tokens, the 80-slot visit table, the slow-port masks and the two commit
strobes — and it runs clean on hardware. The ring is programmed and the chip
stays up. The engine does not advance it: a find-probe on every enrolled port,
both directions, comes back `FOUND=0`, and `0x8062` reads back exactly the
port number written into it. Ruled out along the way: the scheduler tick
(`0xf010`) on its own, the five sweeper words, the twelve MGMT configuration
words, the block clocks `0x1c03a/3b` (already `0xffffffff`, matching EOS), the
SBus init, the memory BIST, and the management token's Sync bit — **our two
earlier generations disagree about that bit and it turns out not to matter**,
which is worth knowing only so that nobody spends another day on it.

And, since: **the ring's contents are not the problem either.** The first
implementation here programmed the five-token bootstrap ring our later
prior-art tool uses. The *golden* ring, recovered from the running switch,
turns out to be a different object — 64 tokens in a fixed service order,
ports 0–3 Locked and sixty others not, one slow-port mask written rather than
five, and the replace-token registers cleared either side of the commit
strobes; 175 writes. `ssched.c` now programs that, and the engine
still does not advance it.

**That is now proven rather than believed.** `ssched.c`'s output was dumped
through a recording stub and compared against the reference sequence:
**175 of 175 writes, same addresses, same values, same order.** The prior
work's own generator says the 64 token values have no formula that fits and
reproduces them verbatim; ours computes them from a port list and lands on
the identical sequence, port 2's out-of-order token included. Removing the
five sweeper words — the one place we write more than the golden sequence
does — changes nothing either. So the ring's contents are eliminated as the
cause, and the remaining difference is state established somewhere else.

**And it is not leftover vendor state either.** Every measurement above was
taken on a board the vendor OS had configured at some point, which leaves
open that it had also left something behind that the ring needs. The mains
were pulled on 2026-09-27; the chip came back genuinely cold — `BOOT_CTRL`
`0x320`, `PIN_STRAP` `0x208`, nothing having run — and after the documented
boot the ring still programs cleanly and still does not advance. That
control is worth as much as any of the positive attempts: it removes the
most comfortable remaining explanation.

⚠ Do not use physical port 0 as a find-probe target. The probe writes the
port number and reads the register back, and for port 0 a ring that never
ran and a ring that answered are both `0`.

### What EOS actually calls, and the boot order it produces

*2026-09-28.* The agent was extracted from the EOS image
(`usr/lib/libFocalPointV2Agent.so`) and its undefined symbols intersected
with the SDK's exports. **EOS calls 286 of the SDK's 2,176 functions**, and
the bring-up part of that is tiny:

```
fmInitialize → fmPlatformHwAccessInitialize → fmPlatformConfigure
             → fmSetSwitchState        (the entire boot)
             → fmSetPortState          (per port, the lane enable)
             + fmPlatformSetRingMode, and the port-mapping calls
```

Everything else in the 286 is ACL, VLAN, LAG, mirror, multicast, FFU and
counter plumbing — the forwarding API, not bring-up.

**And the platform layer is almost entirely the SDK's own.** The agent
registers exactly three platform callbacks — `fmPlatformGetPortCapabilities`,
`fmPlatformMapLogicalPortToPhysical`, `fmPlatformMapPhysicalPortToLogical`.
Everything else, including `fmPlatformSetRingMode` and
`fmPlatformGetSchedulerConfig`, runs the SDK's default. **There is no hidden
Arista bring-up logic**: the sequence we are trying to reproduce is the
SDK's, and that is worth knowing before spending time looking for board
magic that is not there.

The boot `fmSetSwitchState` produces:

```
fmPlatformRelease
fm6000PrebootSwitch     ← fm6000BistMemoryInit, then fm6000MrlRegisterFix
fmDelay
fm6000InitSBus
[the scheduler ring init]   ← calls fmPlatformGetSchedulerConfig
fm6000ValidateSchedulerToken
fm6000InitRegisterCache
fmPlatformLoadMicrocode / ValidateMicrocode
fm6000LoadSpicoCode
[port setup]
```

⚠ **This puts the MRL scan-chain fix inside the pre-boot, before the SBus is
started and long before the scheduler ring is programmed.** It is not an
optional repair step that happens somewhere later; it is the second thing
the switch does. `fm6000MrlRegisterFix` drives exactly `0x1c039`–`0x1c03d`,
as the prior work found, and there are two versions of it selected by an API
attribute.

So the scheduler wall and the MRL are almost certainly the same problem: the
ring is programmed into memories whose configuration has never been shifted
in. That is consistent with everything measured — a ring that accepts its
tokens, reports them back, and never advances.

The scheduler ring itself is built from a **text API attribute**, with modes
the SDK logs as `FMODE_NONE` and `FMODE_MANUAL`, and a string reading
"Automatic scheduler initialization should not happen". So the ring content
is configuration rather than silicon, which is why ours matches golden
byte for byte and still does not run.

### Circulation is not reachable by setting registers at all

*2026-09-27.* The obvious remaining theory was that some register we do not
write holds the scheduler down, and that finding it was a matter of looking
harder. It is not, and the experiment that settles it is worth more than the
list of individual things ruled out above.

Two dumps of a **forwarding** chip exist in the reverse-engineering tree:
`eos-golden-regs-0x0-0x10000.txt` (16,042 words) and
`eos-golden-regs-0x1C000-0x1E000.txt` (190). Between them they are the whole
low register space and the whole of MGMT, as a switch that is passing traffic
holds them. A forwarding chip has thousands of words set in `0x1000`,
`0x4000`, `0x5000` and `0xb000` that NOSaic never writes.

So they were loaded onto our chip after the documented boot — 12,882 words of
low space (the `0x2000`–`0x3fff` egress scheduler excluded, since it is fatal
to touch), then 189 of MGMT, then both together. `BOOT_CTRL` was held back
because writing it re-issues a boot command. Nothing was refused by the
guard, the chip stayed up throughout, and:

```
loaded 12882 words, 0 refused by the guard, chip answering
  circulation: not found -- the ring is programmed but not advancing
```

**Giving our chip a forwarding chip's entire register state does not make the
ring advance.** Circulation is therefore not a register value we have failed
to find. It needs something procedural — a sequence, a shift, or a timing —
and the one procedural step we know about and cannot reproduce is the
scan-chain memory configuration, whose load data is third-party and absent.

That matches what the prior work concluded about the banked memories from the
other direction: *bank writability is a scan program, not a register value.*
The scheduler looks like the same kind of thing.

⚠ The tool used for this, `fm6000-probe --load`, is scaffolding and is
labelled as such in the source: it exists to answer "which block is the
missing precondition?" by putting a known-good chip's state into ours and
seeing what starts working. It is a replay primitive, nothing in `nosd` may
call anything like it, and the answer it gives is a pointer to a block to go
and understand rather than a configuration to ship.

One thing that *was* tried, and did not work: **the scan-chain commit on its
own.** The routine is a per-block load loop followed by a commit and a 20 ms
settle. We cannot have the load data, but if the chip powered up with a valid
default memory configuration the commit alone might have been enough. It is
not — `0x1c039` = `0x10`, `0x1c03a` = `0x80000040`, block clocks restored,
and a deliberate read of `0x2020` kills the chip exactly as it does after a
plain boot. Measured against a control run in the same session. So the load
data matters, and that is the part we do not have.

The scheduler **freelists** are also not it. The prior work left a note that
if the ring turned out to have no queue backing, the freelist init registers
`0x80F0`/`F4`/`F8`/`FC` plus their DONE strobes were the thing to add. They
read zero on our chip — but they are **write-only**, so that reading says
nothing, and the first version of this paragraph wrongly concluded from it
that boot command 3 had not run. What settles it is the capture: the working
switch **never writes those registers at all**. It relies on boot command 3,
which we also run. Writing INIT=0 and DONE=1 to all four changes nothing.

(That comparison needed `fm6000-probe --read-unsafe`, which takes a hazard
the guard refuses. There is no way to learn whether a block has become
reachable except by reading it, and on this chip that read is what kills a
chip where it has not — so the tool asks anyway, says it is doing so, and
reports whether the chip survived, which is the result either way.)

Still untried, and the leading candidate: the scan-chain memory configuration
(`0x1c039`–`0x1c03d`). Our prior art found that bank writability is a scan
*program* rather than a register value, and that direct writes off-bus without
it. We cannot reuse its table — that project deliberately kept the scan-config
values out of its own tree as third-party data loaded at runtime, so there is
nothing there to inherit even setting licensing aside. The handshake itself is
short and is described in `todo.md`.

**What the code does about it now.** `fm_hazard()` refuses the whole
`0x2000`–`0x3fff` block, in both directions, until `fm_sched_mark_ready()` —
which only `ssched.c` calls, and only on a find-probe that actually came back
found. `fm_esched_init()` is written, its 159 addresses cross-checked against
the reference table, and it writes nothing at all today: it reports

```
egress scheduler: 0 writes, refused as unsafe
  ESCHED: the scheduler ring is not circulating, so nothing in this block
  can complete an access. Touching it takes the chip off the bus.
```

which is the correct behaviour for a block whose precondition is not met.

### Which blocks actually keep what we write

Every ported block now proves its own writes landed, because on this chip a
write that returns ok is not a write that stuck:

| block | writes | verified |
|---|---|---|
| store-and-forward | 168 | ✅ a front-panel entry reads back `0x0010000f` |
| congestion watermarks | 6512 | ⚠ **4 of 6 tables**; both TX tables keep nothing |
| CM maps, pause, partitions | 1005 | ✅ all six regions spot-checked |
| parser seed clear | 194 | ✅ poked `0xdeadbeef` first, cleared after |
| egress scheduler | 0 | refused — the block is unreachable |

⚠ The witness value has to be distinctive. The obvious choice for
store-and-forward was the CPU port's entry, which is `0xffffffff` — and so is
an untouched register, so a block that discarded every write would have
passed. A plain front-panel port's first word is `0x0010000f`, which nothing
produces by accident.

## What the datasheet says about the EPL, and what we were doing instead

*2026-09-28, from Intel 331496-002 chapter 6.* Four findings, of which one
was a bug we were shipping, two are latent bugs we would have hit on the
next 44 ports, and one reframes the receiver problem.

**§6.8.9 — a PCS mode change must go through `PCS_DISABLE`.** "The correct
method is to first set to PCS_DISABLE and then change to the desired mode."
`serdes.c` did it in one read-modify-write, straight from whatever the
selector held to 10GBASE-R. That happens to start from disable on a freshly
booted chip and does not on a port being reconfigured, so it worked exactly
often enough not to be noticed. Now two writes, and the first is not
redundant. **It did not fix the receiver** — as expected, since our test
always started from a fresh boot.

**§6.2.3 Table 6-2 — ten of the 24 EPLs have their lanes reversed inside the
package.** For EPLs 1, 2, 4, 8, 12, 13, 17, 20, 22 and 24, external lanes
{A,B,C,D} reach internal channels {3,2,1,0}. Every port we have ever brought
up is on EPL 14 or EPL 16, both straight-through, so a lane index that
ignores this works on everything tested and fails on the other forty-four —
as a port that configures cleanly and never links. ⚠ It is also **not yet
known which index the board's FDL gives us**, because on EPL 14 and 16 the
two are identical. `fm6000_epl_lane_reversed()` and `fm6000_epl_channel()`
hold the table; the first port brought up on a reversed EPL settles the
question.

**§6.4 Table 6-7 — one reference clock per six EPLs**, and every port this
port has ever tested is on ETH_REFCLK4: panels 1–8 are EPL 14 and EPL 16,
which are in the same group. That is a blind spot worth knowing about.

It is **not**, however, the explanation for the receiver, and I nearly
filed it as one. Step 6 of the lane enable waits for SerDes PLL lock and
gets it, and that PLL is derived from the reference clock — so the clock
reaches the SerDes and the PLL works. A dead ETH_REFCLK4 would have failed
at step 6, not at step 12. Worth writing down because the blind spot is
real and the conclusion it invites is wrong.

**§6.11 Table 6-15 — for 10GBASE-R the only *required* link condition is
block lock.** SerDes Ready, SerDes Signal Detect and Idle Detection are all
optional and software-selectable. This matters because the receiver has
been diagnosed for weeks off "signal detect never asserts" — and signal
detect is both optional as a link condition and, as this port already
established, reads the same on a forwarding lane as on a dark one. The
diagnosis rests on an indicator that carries no information.

### The lane that will not receive, compared against one that did

There are register dumps of this chassis' SerDes taken while their ports
were **up**, in the reverse-engineering tree's `scd-dumps/`. Diffing a lane
that will not come up against one that did is the only way left to look
inside a part whose register set is in no public document.

**The EPL per-lane registers.** Ours after a full bring-up, against Et1 up:

| | ours | working |
|---|---|---|
| `PORT_STATUS` `+0x00` | `0x00000815` | `0x00000ac0` |
| `PCS_RX_STATUS` `+0x26` | `0x00000000` | `0x00000001` (block lock) |
| `SERDES_CFG_lo` `+0x34` | `0x0aaaa005` | `0x0aaa86c0` |
| `SERDES_CFG_hi` `+0x35` | `0x00000000` | `0x00000001` |
| `LANE_CFG` `+0x37` | `0x00000001` | `0x000c0002` |
| `LANE_STATUS` `+0x38` | `0x00000000` | `0x00000940` |

Three of those our bring-up never writes at all, and one of them looked
like the answer: `SERDES_CFG_lo` carries **RefSel** in bits [11:6], and ours
is `0x00` where a working lane has `0x1b`. The prior work on this chassis
had marked that same register "*** THE MISSING REG ***".

**It is not sufficient.** Writing all three to the working values — verified
to stick, tried both after the bring-up and before it, with the values
surviving the bring-up — leaves `LANE_STATUS` at zero and no block lock.

**The SBus registers.** Our SerDes `0x49` against the same device on a chip
with three ports up: **35 of 256 registers differ**, including `0x19`–`0x27`
which is populated there and entirely zero here, with a repeating
`0e 61 f0` at `0x21`–`0x23` and again at `0x25`–`0x27` that looks like two
equaliser coefficient sets.

⚠ **But SBus register reads do not return what was written.** Writing reg 31
`= 0x29` reads back `0x00`; writing reg 3 `= 0x01` reads back `0xaa`. That
is not a stuck bus — the values are stable and plausible, they are simply
not the ones written. Which means two things: the dump above cannot be read
as "the state a working lane is in", and more seriously, **the
read-modify-write that the whole lane bring-up is built on is computing
from a base that may not be what it thinks.**

Writing the working lane's 35 differing values directly does not bring the
lane up either, which is consistent with the same thing.

Reading three times in a row gives `0x00` every time, so it is not a
one-transaction lag either. **The SBus read and write paths address
different register sets** — which the prior work on this chassis had already
written down: *"readback proves nothing on this bus; the acceptance test is
behavioural, PORT_STATUS bit 11."* A read dump is a symptom comparison and
never a recipe.

That also retires the alarm I raised about `rmw()`. Reads give the current
state of the read space; they simply do not confirm a write. The
read-modify-writes are not computing from fiction.

### Step 7 was never finished: SPICO stays in reset

There is a **vendor register header** in the reverse-engineering tree,
`reference/fm6000-sdk/fm6000_api_regs_int.h` — 9,500 lines of named
registers and bit positions. It is facts about the silicon, and it is far
better than disassembling the SDK for the same information.

It confirms our SBus command layout exactly (`Register[7:0]`,
`Address[15:8]`, `Op[16:]`) and names `FM6000_SBUS_SPICO` at JSS + 0x04 —
which is `0x0f004`, one of the two registers a forwarding chip has set that
we never wrote. Its bits: 0 Reset, 1 Enable, 2 Interrupt.

**After our documented boot it reads `0x1` — the SerDes micro-controller is
held in reset.** Table 4-1 step 7 says "take all modules out of reset (EPL,
PCIe, MSB, SPICO/SBUS)", and driving `SOFT_RESET` to zero does not do it:
this is a separate bit in a separate register. Step 7 was half implemented
and nobody noticed, because nothing we do afterwards needs SPICO.

⚠ **Clearing it during step 7 takes the chip off the bus.** Measured twice,
reproducibly, from a verified-recovered chip each time. It is survivable in
`fm_sbus_start()`, after the SBus controller itself is out of reset — which
is the order the dependency implies anyway, since SPICO sits on the bus that
function has just started. That is where it now happens, and
`SBUS_SPICO` reads `0x00000000` afterwards: out of reset, not running,
§9.4.1's middle state and the only one in which code can be downloaded.

Enable is left alone. A forwarding chip reads it set, and that chip has
firmware in it; enabling a micro-controller with no code is not something to
do because a working chip's register says so. Setting it by hand changes
nothing on the receiver, which is the expected result and was checked.

### Two more facts from the vendor material, and one dead end

**There is a memory-mapped SerDes register path, with separate read and
write windows.** The header defines `FM6000_SERDES_ETH_WRITE_BASE 0xB0500`
and `FM6000_SERDES_ETH_READ_BASE 0xC0500`, each `0x10000` words. That is the
clean explanation for the read/write asymmetry on this bus: they are
genuinely different address spaces, not one register behaving oddly.

⚠ It is not usable yet. Reading the obvious address for our lane —
`0xC0500 + (index << 8) + reg`, and three other indexings — returns zero for
every register, while the SBus path returns sensible values for the same
lane. The chip stays up throughout. So the window exists, is documented, and
something about reaching it is not understood. Not pursued further, because
our SBus writes demonstrably work: `SerXmit` is the proof.

**The SDK's own mapping chain**, from the vendor Python HAL, is
`physical → (EPL, channel) → lane → serdes`:
`fm6000PhysicalToEplChannel`, then `fm6000EplChannelToLane` — which is
Table 6-2's reversal — then a search over `fm6000SerdesToEplLane`. The
tables themselves are inside the SDK binary, so this gives the shape and not
the numbers, but the shape confirms two things we had inferred: the lane
reversal is part of the real addressing path and not a footnote, and
serdes ↔ EPL is a permutation rather than arithmetic. EPL 14 lane 0 is
serdes 68 — measured here, and stated outright in the golden dump's own
header — where `(14-1)*4 + 0` would be 52.

### Static analysis of the SDK: the "undecoded arithmetic" is a table

*2026-09-28.* `libFocalpointSDK.so` is a 32-bit ELF **with full symbols** —
987 `fm6000` functions, including every step the prior work could not
decode. `fm6000EnableSerDes` is at `0x48131e`, exactly where that work said.

**The SBus addressing**, which also settles the memory-mapped windows:

```
addr = (serdes_index << 8) + 0xB05RR     Ethernet
addr = (serdes_index << 8) + 0xD11RR     PCIe
```

with `RR` the register number — so `0xB0500`/`0xD1100` are bases and the
index is the SerDes number, not the SBus device id.

**The eleven registers it touches**, in order: `0x22`, `0x00`, `0x1d`,
`0x36`, `0x3b`, `0x17`, `0x22` again, `0x06`, `0x03`, `0x1f`, `0x26`,
`0x0d` — each a read-modify-write except `0x1d`, which is a plain write.
That matches the shape of our implementation.

**And steps 3–6 are not arithmetic.** They are a four-entry table selected by
the port's line rate in Mb/s:

| rate | reg `0x00` bits [6:1] | regs `0x36`, `0x3b` |
|---|---|---|
| ≤ 1250 | `0x13` | `0x63` |
| ≤ 3125 | `0x01` | `0x06` |
| ≤ 6250 | `0x01` | `0x09` |
| otherwise | **`0x1b`** | **`0x40`** |

Those thresholds are the SerDes rates of §6.5 — 1.25, 3.125, 6.25 and
10.3125 GbE — so the last row is 10G, and **it is exactly what our code was
already writing.** Register `0x00` gets that value in bits [6:1] with bit 0
set, which is precisely `(0x1b << 1) | 1`; `0x1d` gets a plain zero. So the
values were right and what was missing was any reason to believe them.
`serdes.c` now names them `FM_SERDES_RATE_DIV` and `FM_SERDES_RATE_WIDTH`
and says where they come from.

**One supposed missing step does not exist.** `fm6000SetSerDesRxDataGate` is
a **stub** — it stores its argument and returns 0. Nothing to implement, and
one fewer candidate for the receiver. `SetSerDesKrTraining`, `SetTxConfig`
and `StartSerDesDfeTuning` are real functions and remain unimplemented.

**Step 2, KR training, is now implemented.** `fm6000SetSerDesKrTraining` is
one register: `0x5a`, clear bit 1 and set bit 0. The vendor's lane enable
calls it with the "off" argument, after the receive datapath is held down
and before the rate is selected, and that is where `serdes.c` now does it.
KR training is the backplane-copper negotiation and this board is SFP+
fibre, so it has to be off — and the default is not knowable by reading,
since the read and write spaces differ.

**Step 14, SetTxConfig, we already had.** It writes registers `0x0b`,
`0x3d`, `0x3e` and `0x41` — decimal 11, 61, 62 and 65, which are our
polarity and transmit-equaliser steps. Nothing missing, and TX-side anyway.

### What that leaves — and NOT what I first said

⚠ **The paragraph that stood here claimed the SPICO firmware question had
reopened, and that was wrong.** It is retained below only as the reasoning,
because the correction matters more than the claim.

The prior work on this chassis did not settle the SPICO question by
inference. It settled it by **moving `fm6000_spico_code.bin` aside and cold
booting**, and measuring:

| port | media | PORT_STATUS / LANE_STATUS | outcome |
|---|---|---|---|
| et1 | SFP fibre, 10GBASE-SR | `000008c0` / **`00000940`** | **clean lock, forwards** |
| et2 | DAC copper, 10GBASE-CR | `00000815` / `00000000` | no lock |

et1 carried traffic end to end with **zero Intel code present** — 5/5 pings
from the peer, OSPF adjacency up, 14 routes programmed. A fibre-only build
needs no Intel firmware, which is exactly what makes this board
distributable.

So the receiver failure is **not** the missing DFE step, and the cage we are
trying to light is fibre. What it is, is a difference between our
implementation and one that demonstrably worked on this same chassis with no
firmware — which is a far better place to be than waiting on a blob.

Two measurements from that same test worth carrying, both taken on a
no-firmware box: **reg `0x0f` reads `0x3f` and reg `0x14` reads `0x14` on
both a locking and a non-locking lane.** Our lane reads the same. So neither
PLL lock nor signal detect distinguishes a working lane from ours, which
retires them as diagnostics for the third time.

### The reasoning that was wrong, kept as reasoning

With KR training added, **every step of the vendor's lane enable is now
implemented except one**: steps 17–18, the DFE tuning. And that one is not a
hardware engine — the prior work established it is a **mailbox to the SPICO
micro-controller**, which answers only when firmware is loaded into it.

The lane still does not lock.

The inference was: everything but the DFE is implemented, the DFE needs
SPICO, the lane does not lock, therefore the lane may need SPICO. It is
sound reasoning from a false premise — that the firmware question had been
settled by bisect. It was settled by removing the file, and the answer was
measured, not argued.

⚠ None of the SDK analysis made the lane lock. That is the point of writing
it down: steps 3–6 and step 2 are now eliminated as the cause rather than
suspected, which is worth more than another value to try.

### So what is actually missing, and it is not a mystery

The lane bring-up is the SDK's 18-step `fm6000EnableSerDes`. The prior work
recovered it by disassembly and **says in its own header which steps it
could not decode**:

| step | what | state |
|---|---|---|
| 2 | KrTraining off | not decoded |
| **3–6** | **regs `0x00`, `0x1d`, `0x36`, `0x3b`** | **"values produced by arithmetic in the SDK that has not been decoded"** |
| 14 | SetTxConfig | not decoded |
| 17–18 | DFE tuning | a SPICO mailbox, not a hardware engine |

Our `serdes.c` writes steps 3–6 anyway, with values chosen here. Two of them
disagree with a working lane's read-space state — reg `0x1d` reads `0x08`
against `0x01`, reg `0x17` reads `0xcf` against `0xc0` — and since writes do
not read back, we cannot tell whether that is the wrong value or merely a
different view.

**That is the receiver wall stated properly: it is not a register we have
failed to find, it is arithmetic inside the vendor SDK that nobody has
decoded.** It is the same class of blocker as the scan-chain load data
behind the scheduler — and unlike that one, this arithmetic is a
computation over known inputs (rate, reference divider, lane), so it is the
more tractable of the two.

### The SBus device map: two numbering spaces, and a test that tells them apart

Datasheet §9.4.3 Table 9-4 gives each EPL four consecutive SBus addresses,
and `sbus.c` transcribes it. The port table in `serdes.c` carries its own
device per port, and the two **disagree**: the port table gives EPL 14
lane 0 the device `0x49`, which Table 9-4 assigns to EPL[24]; it puts
EPL[14] at `0x29`.

That looks like an obvious bug with an obvious fix — derive the device from
`fm_sbus_epl_base()` and have one source of truth. I made that change. It
is wrong.

**Configuring SerDes `0x49` makes EPL 14 lane 0 assert SerXmit
(`PORT_STATUS 0x815`); configuring `0x29` leaves it clear (`0x015`).** The
transmitter only comes up when the right SerDes is configured, so the port
table's value is correct and the two numbers live in different spaces: the
EPL register blocks are indexed in one order, the SBus ring is wired in
another — the datasheet says so itself, *"the order on the ring is physical,
not numerical"* — and the board's FDL numbers EPLs in the register space.

Worth recording for two reasons. The note that had justified `0x49` said it
was "confirmed on hardware" because lanes are consecutive within an EPL —
which is true of both candidates and confirms nothing. And the tidy-up is
attractive enough that somebody will try it again; the comment on
`fm_port_sbus_dev()` now says what happens when they do.

So the answer to "where is datasheet EPL[n] on the ring" and the answer to
"which SerDes belongs to this port" are different questions, and only the
second one matters to a port coming up.

### What the datasheet did not settle

The EPL register map is not in it, so the per-lane configuration still has
to come from measurement. Comparing ours against a forwarding chip:

| | ours | forwarding |
|---|---|---|
| `EPL_CFG_A` | `0x0c7d7899` | `0x7e1d7899` |
| `EPL_CFG_B` | `0x00090003` | `0x00090033` |

`CFG_B` is right — the difference is lane 1, which EOS had up and we do
not. `CFG_A` differs in four upper bits, 25 and 28–30.

⚠ **I set those four bits to see what would happen. That was bit-guessing
on undocumented fields, which this port's own rules forbid, and I should
not have done it.**

⚠ **And the result I reported from it was wrong.** I recorded that `SerXmit`
dropped — reading `PORT_STATUS` at `0x0e3400`. EPL 14's lane base is
`0x0e3800`, so `0x0e3400` is **EPL 13**, and what I read was a different
EPL's status word, which is `0x15` whatever EPL 14 is doing. The experiment
established nothing in either direction. Its only lasting product is this
warning about the address, which cost two wrong conclusions before it was
noticed.

The per-lane addresses, since getting them wrong is evidently easy:

| | |
|---|---|
| EPL *n* lane *l* base | `0x0e0400 + (n-1)*0x400 + l*0x80` |
| EPL 14 lane 0 | `0x0e3800`, **not** `0x0e3400` |
| `PORT_STATUS` | base + `0x00` |
| `LANE_STATUS` | base + `0x38` → `0x0e3838` |
| `EPL_CFG_A` / `_B` | `0x0e3b01` / `0x0e3b02` (these were right) |

## The GLORT assignment, settled 2026-09-28

The forwarding path does not address ports by their physical number; it uses
a GLORT. Nothing in NOSaic had one, which blocked the parser's per-port
seeds and everything downstream of them — `LBS_CAM`, the L3AR slices — and
was recorded as "a forwarding decision that belongs with the forwarding
bring-up".

It is simpler than that. **A port's GLORT is its logical port number:** a
front-panel port's panel number, 1 to 52; the two internal ports continuing
as 53 and 54; the host port 0.

Two independent things say so. `portmap.h` has recorded since the port map
was recovered that the vendor's agent log numbers the two internal ports
"53 and 54 in its own logical space" — and that logical space turns out to
be the GLORT space. And a forwarding chip's parser seeds carry exactly this
assignment: checked on all 52 front-panel ports, **51 match exactly** and
the 52nd matches in its GLORT and its low half, differing only in a field
that holds link state.

So NOSaic uses it because it is the right answer, reached twice, rather than
because it is what was there. `fm6000_glort_of()` in `portmap.h` is the one
place it is written down.

The seed layout is

```
word 0   flags << 16 | 0x100 | glort      (the low half is zero on an
word 1   glort << 16 | 1                   internal port)
```

with flags `0x0001` on a port that has not come up. `parser.c` now writes
the whole 304-word array — a seed for the 55 ports that carry traffic, zero
for the 21 that do not, zero in the unused second entry everywhere — and it
matches a forwarding chip on **303 of 304 words**. The one difference is
port 40's flags field, which holds link state and is not a seed.

### Loopback suppression, the first block written from the GLORT rule

A frame flooded to a VLAN must not go back out of the port it arrived on.
The chip decides that per port, by matching the frame's source GLORT against
one word at `0x014000 + port`, packed as its own complement so the match is
exact:

```
entry = glort << 16 | (~glort & 0xffff)
```

`lbs.c` computes all 55 entries from this board's port map and the GLORT
assignment. **Nothing in it is transcribed** — and checking it against the
reference table afterwards gives 55 of 55 identical, the host port included.

⚠ **The host port matches a GLORT no frame carries, not its own.** Its GLORT
is 0, and an entry matching 0 would match every frame whose source GLORT is
unset — quietly suppressing flooding to the CPU. It is given `0xff00`, which
is outside the assignment. That is the rule, not an exception to it, and the
reference table agrees.

⚠ **`LBS_PROFILE_TABLE` at `0x014080` is not written.** Twelve entries
holding 0 or 2 in a pattern nothing here explains. Writing twelve values
because a reference has them is the thing this port does not do; it waits
until someone can say what a profile is.

### The egress scheduler, checked against a chip that forwards

The block cannot be written on our switch, so `esched.c` could not be tested
the way every other block was. The golden dump closes that: it contains the
whole `0x2000`–`0x3fff` block as a **forwarding** chip holds it, so what we
generate can be compared against what works, offline.

The first comparison was reassuring and incomplete: **all 159 addresses we
wrote agreed exactly, none disagreed** — and the forwarding chip had 3,001
more that we never touched.

Those turned out to be completely regular. The block is **eight instances of
four arrays**, `0x200` apart, each array one word per physical port across
all 128:

| | |
|---|---|
| arrays 0, 1 and 3 | filled; port 0 special, every other port `0x00ffffff` |
| array 2 | zero in all eight instances — which is what our earlier tool wrote as an explicit `CFG_3 = 0` |
| port 0, arrays 0 and 3 | `0x00fff800` |
| port 0, array 1 | `0x00fff000` |

⚠ **And the round-robin word is not what we had.** A forwarding chip writes
all 76 switch ports and leaves **exactly two** still carrying the
inter-frame-gap penalty: physical ports **1 and 3**. Those are the two ports
with no cage — the same two the store-and-forward table singles out, arrived
at from a completely different direction. Our version settled everything and
never wrote ports 1 and 3 at all, which is not a tidier way of doing the
same thing; it is configuring the switch differently from one that works.

`esched.c` now produces 3,222 writes over 3,148 addresses, and diffing that
against the forwarding chip gives **zero disagreements and zero extras**.

**⚠ And the model above was wrong**, which the vendor's register header
settled. The ESCHED block contains **three registers, 76 entries each** —
nothing else:

| register | address | fields |
|---|---|---|
| `ESCHED_CFG_1` | `0x2000 + port` | `prioritySetBoundary[11:0]`, `tcGroupBoundary[23:12]` |
| `ESCHED_CFG_2` | `0x2080 + port` | `strictPriority[11:0]`, `tcEnable[23:12]` |
| `ESCHED_CFG_3` | `0x2100 + port` | `tcInnerPriority[11:0]` |
| `ESCHED_DRR_Q` | `MONITOR + port*0x10 + class` | 12 × 76 |
| `ESCHED_DRR_CFG` | `MONITOR + 0x800 + port` | |

So the "eight instances of four arrays over 128 ports" was **address
aliasing**: the block decodes only part of the address and everything above
`0x2180` is those same three registers seen again. The dump was 24 views of
one array, and the model built from it wrote 3,222 words where the hardware
has 304 registers. `esched.c` now writes 378.

Two other corrections fall out. **The field names were on the wrong
register** — what this file called CFG_1's `strictPriority`/`tcEnable` is
CFG_2's layout. And **the 12 unexplained words at `0x3000`–`0x300b` are
`ESCHED_DRR_Q[port 0][class 0..11]`**, the per-class deficit counters: not a
configuration at all, but what that chip's scheduler had reached at the
moment of the dump. Correctly not written, now for a reason rather than out
of caution.

⚠ Diffing against a forwarding chip will now show it holding values at
addresses we do not write. Those are the aliases.

### ⚠ The transmit watermark tables are NOT blocked — that was my mistake

*Corrected 2026-09-28. The section that follows is kept because the
measurements in it are real and the conclusion drawn from them was wrong.*

`fm_cmwm_init()` writes 6,512 words across six tables. Four read back what
was written; `TXMP_PRIVATE` and `TXMP_HOG` read zero. I concluded that those
two accept writes and keep nothing, called it the scheduler wall showing up
silently on the egress side, and wrote it into the code as a reported fault.

The golden capture settles it the other way: **on a chip that is forwarding
traffic, those two tables also read entirely zero**, while `RXMP_HOG`
immediately beside them reads all 1,216 of its words. They are write-only.
Reading zero from them says nothing at all, on any chip.

So there is no evidence the writes fail, the CM configuration is very likely
complete, and the wall blocks one block — the egress scheduler — not two.

This is the same mistake twice in one session. I caught it for the scheduler
freelists, wrote down that a zero read of a write-only register proves
nothing, and then did not go back and apply it here. The reporting now
distinguishes the two answers, because "I checked and it is wrong" and "I
cannot check" send people to very different places:

```
congestion watermarks: 6512 words, accepted
  4 of 4 tables verified; 2 are write-only and cannot be checked
```

What the measurements below do establish is the *behaviour* — which tables
read back and which do not — and that part stands.

### The original reading: the transmit watermark tables

The egress scheduler at least fails loudly, by taking the chip off the bus.
The congestion-management watermarks fail the other way.

`fm_cmwm_init()` writes 6,512 words across six tables in the CM block. All
6,512 are accepted, nothing errors, and the chip stays up. Reading them back:

| table | | |
|---|---|---|
| `RXMP_PRIVATE` `0x112800` | reads back what was written | ✅ |
| `RXMP_HOG` `0x113000` | reads back what was written | ✅ |
| `TXMP_PRIVATE` `0x113800` | **reads `0`** | ❌ |
| `TXMP_HOG` `0x114000` | **reads `0`** | ❌ |
| `RXMP_PAUSE_ON` `0x115000` | reads back what was written | ✅ |
| `RXMP_PAUSE_OFF` `0x115800` | reads back what was written | ✅ |

Poking `0xa5a5a5a5` into one word of each confirms it: `0x112800` holds it,
`0x113800` and `0x114000` come back `0`. Same block, same access path, same
instant — the receive-side tables store and the transmit-side tables discard.

That is the scheduler wall again, on the egress side of the chip, and it is
worse than the ESCHED symptom because nothing announces it. A count of words
written proves nothing here, so `fm_cmwm_init()` does not report one on its
own: it re-reads a known word from each table afterwards and reports how many
verified, plus the first that did not.

```
congestion watermarks: 6512 words, accepted
  4 of 6 tables verified; first not to take: TXMP_PRIVATE
```

*(The conclusion that followed here — that the two tables were blocked by
the scheduler and would need re-running once the ring circulates — is
withdrawn. See the correction above: they are write-only, and a forwarding
chip reads them zero as well.)*

### Every block checked against the part's own register names

*2026-09-28.* The ESCHED correction raised the obvious question: what else
was authored from inferred geometry? So all of them were checked against
`fm6000_api_regs_int.h`, which names every register, its address, its entry
count and its bit fields.

| block | verdict |
|---|---|
| `saf` | ✅ `SAF_MATRIX(index, word) = 0xA0000 + 4*index + word`, 76 × 3 — exactly ours, pitch included. Bit 0 is `EnableSNF`, bits 80–81 `CutThruMode` |
| `lbs` | ✅ `LBS_CAM = 0x14000 + index`, 76 entries; `LBS_PROFILE_TABLE = 0x14080`, 16 — exactly ours, and the profile table we declined to write is 16 entries as observed |
| `cmwm` | ✅ all six tables, addresses *and* dimensions: RXMP private/pause 12 × 76, RXMP hog 16 × 76, TXMP private/hog 16 × **80** — the asymmetry we found by measurement is the documented shape |
| `cmrest` | ⚠ addresses and values right, **names invented and two of them misleading** |
| `esched` | ❌ corrected above |

`cmrest` is renamed to the part's own vocabulary. What it called a "hash"
at `0x112200` is `CM_GLOBAL_WM`, the global watermark; what it called a
"partition" map is `CM_TC_PC_MAP`, traffic-class to port-class. The six
words at the base of the block are not one table but three two-word maps —
`CM_RXMP_MAP`, `CM_TXMP_MAP`, `CM_TC_MAP` — which is why they looked like a
header followed by two repeating pairs. The rest: `CM_BSG_MAP`,
`CM_PC_RXMP_MAP`, `CM_SHARED_RXMP_WM`, `CM_RXMP_SOFT_DROP_WM`,
`CM_SHARED_RXMP_PAUSE_ON/OFF_WM`, `CM_PAUSE_CFG`.

No address or value changed, and it still diffs 701 of 701 against a
forwarding chip. What changed is that the file now says what it is
configuring.

### Congestion management, the parts that do work

`cmrest.c` is the rest of the CM block: the PAUSE configuration, the class
and partition maps, and the shared-partition thresholds. 1,005 writes over
701 addresses — the PAUSE table is written twice, whole-chip, every port
parked before any port is started — and all ten spot readbacks match, across
all six regions. Nothing here is scheduler-gated.

Both CM files are regenerated from geometry and this board's port map rather
than transcribed, and both were checked against the reference before going
near the switch: `cmwm` matches all 6,512 (address, value) pairs, `cmrest`
all 1,005 **in order**, which for `cmrest` matters because its PAUSE table is
two-phase.

The port grouping in every one of these tables is the same question — is this
a front-panel port, is it the host port — so it is asked of `portmap.h` once
rather than written out as ranges. A board with a different map gets the
right answer without anybody editing a table.

## The scan chain and the MRL register fix

Recovered 2026-09-28 by static analysis of `fm6000MrlRegisterFix` in the
vendor SDK. Nothing from the vendor binary is copied into NOSaic; what is
recorded here is how the hardware is addressed, which is fact, and the code in
`datapath/fm6000/mrl.c` is our own.

### This is not Table 4-1

The two accounts that were in conflict — the datasheet says step 5 is a single
write of `0xFFFFFFFF` to `SCAN_CHAIN_DATA_IN`, the prior investigation says a
several-thousand-step scan program is needed — are both correct. They describe
different operations:

- **Table 4-1 step 5** is one write, and **step 9** ("Apply Bank Memory
  Repairs") is a `BOOT_CTRL` command the boot controller executes by itself.
  `boot.c` does both, and has since 2026-09-25.
- **The scan program** is an erratum workaround applied afterwards, by the CPU,
  gated on a chip-revision equality test — the vendor skips it on every
  revision but one. It is not in the datasheet because it is not part of the
  documented boot.

### The register window

The five registers at `0x1c039`–`0x1c03d` are one shift-register port, not five
independent registers:

| Address   | Name                    | Role |
|-----------|-------------------------|------|
| `0x1c039` | `SCAN_SELECT`           | chain selector, `[4:0]` |
| `0x1c03a` | `SCAN_CONFIG_DATA_IN`   | engine command port |
| `0x1c03b` | `SCAN_CHAIN_DATA_IN`    | chain data port |
| `0x1c03c` | —                       | in the window, never accessed |
| `0x1c03d` | `SCAN_STATUS`           | shift status, `[9:8]` |

Every shift is: write the selector, write one 32-bit word to whichever data
port the chain wants, read the status. `[9:8] == 01b` means the word retired.
The selector is rewritten for *every* word even when unchanged — on a
shift-register port that write may be what clocks the previous word through, so
it must not be hoisted out of a loop. Anything but `01b` is re-read once before
being called an error; that re-read is part of the protocol, not a retry loop.

Only two selectors are ever used: `0x10` (core) and `0x14` (banks). The field
is five bits wide, so thirty more exist that nothing has touched.

### The sequence

6287 shifts, plus a stop word after the loop:

| Part | Shifts | Port | Chain |
|------|--------|------|-------|
| prologue | 35 | config | core |
| bank chain | 5800 | data | banks |
| core chain | 203 | data | core |
| tail | 249 | mixed | core |
| stop | 1 | config | core |

The prologue is 21 zero words with the engine stopped, then `START`, then three
parameters, then ten pump words numbered 0–9. The tail is three groups of sixty
pumps each followed by an end marker, then a fourth group of sixty-six with no
marker. The end marker is `0xfffffff8` — the same all-ones word step 5 writes,
with the low three bits clear; what those three bits mean is not known.

Engine commands appear to encode an opcode in `[31:24]` and an operand in
`[7:0]`: `0x80` start/stop, `0x84`/`0x85`/`0x86` three parameters set once,
`0xbf` advance. That split is inferred from the bit pattern and from the fact
that only those six opcodes ever appear. It is not documented.

### What we do not carry

The 5800 + 203 words of chain payload are third-party data and NOSaic does not
ship them. `fm_mrl_apply()` shifts a payload the caller supplies and the caller
that supplies nothing shifts zeros, which `mrl_test.c` enforces.

That is a usable experiment rather than a stub. For the record, because it
bears on whether the payload could ever be *derived* rather than copied: it is
not dense data. Of 6003 words, 185 are non-zero, every non-zero value is a
five-bit field at bit 15, and they fall into four clusters that are each
internally periodic with a period of 25 words. Two of the four clusters are
rotations of the other two. Whatever the values mean — and we do not know —
the payload carries on the order of a hundred numbers, not fifty kilobytes.

### What running it with zeros actually did

Measured on the lab 7150S on 2026-09-28, twice, with a cold boot in between:

| | result |
|---|---|
| `--ssched` | ring initialises, does not circulate, **chip answering** |
| `--mrl` | all 6288 shifts retire, **chip answering** |
| `--ssched` | *the same command* takes the **chip off the bus** |

Two things follow.

**The sequence reaches something the scheduler depends on.** That coupling was
the whole reason for chasing this and it had never been demonstrated; it is now
measured. Every shift retiring (`SCAN_STATUS[9:8] == 01b`, 6288 times) also
says the protocol decode above is right — a wrong selector or a wrong data port
would not retire.

**The payload is load-bearing, and a zero payload is destructive.** Shifting
zeros evidently overwrites configuration that something the scheduler needs was
relying on. The precise mechanism is *not* established, and an earlier guess
here — that step 9's fuse-derived bank repairs were being overwritten — is
probably wrong: MRL on this chip is the **metering rate limiter**, the policer
sweeper, not memory repair. The SDK's own register names say so
(`FC_MRL_SWEEP_CYCLES`, `FC_MRL_UNROLL_ITER`, `FC_MRL_RATE_LIMITER`) and so
does the datasheet, which uses MRL only for the policer sweeper (§5.13.5).

So `fm6000MrlRegisterFix` is a scan-chain patch of the rate-limiter block's
registers, and it is *not* a missing bank-repair step. That is worth saying
plainly because it was the reason for chasing it. What survives is the measured
coupling: something on these scan chains is load-bearing for the ring.

Recovery is a reset pulse (`nosaic platform release-asic`) and a full `--boot`.
`fm6000-probe --mrl` therefore refuses to run without `i-mean-it`.

**Where this leaves it.** Not on running the sequence empty, and probably not
on this sequence at all — a policer erratum patch is unlikely to be what makes
a segment scheduler circulate. The sequence is implemented and documented so
that it is available and so that nobody has to decode it twice, but the
scheduler lead has moved on; see the register map below.

## The register map, and how to regenerate it

The vendor SDK carries a table of 720 entries, 56 bytes each, at file offset
`0x3ac790` in `libFocalpointSDK.so` (EOS 4.16.8M). Word 0 of each entry is a
pointer to the register's name and **word 2 is its word address**. 703 of the
entries are `FM6000_*` registers spanning `0x00000`–`0x3fc400`.

This is a register map — names and addresses — of the same kind the datasheet
publishes for the blocks it covers. It is not code and it is not captured
state, so it is used here the way the datasheet is: to find and name registers.
It is **not** committed to the tree. Regenerate it with:

```python
# .rodata is file 0x313040 -> vaddr 0x513040, so vaddr = offset + 0x200000
import struct
d = open("libFocalpointSDK.so", "rb").read()
def name(va):
    off = va - 0x200000
    return d[off:d.index(b"\0", off)].decode()
for i in range(720):
    w = struct.unpack_from("<14I", d, 0x3ac790 + i * 0x38)
    print("%08x  %s" % (w[2], name(w[0])))
```

### Why it can be trusted

Every address this port had already established the hard way, on live hardware,
appears in the table and matches — ten for ten:

| Address | We measured | Table says |
|---------|-------------|------------|
| `0x00009` | `SOFT_RESET` | `FM6000_SOFT_RESET` |
| `0x0f000` | `SBUS_CFG` | `FM6000_SBUS_CFG` |
| `0x0f001` | `SBUS_COMMAND` | `FM6000_SBUS_COMMAND` |
| `0x1c022` | `BOOT_CTRL` | `FM6000_BOOT_CTRL` |
| `0x1c039` | scan selector | `FM6000_SCAN_CONTROL` |
| `0x1c03a` | `SCAN_CONFIG_DATA_IN` | same |
| `0x1c03b` | `SCAN_CHAIN_DATA_IN` | same |
| `0x1c03d` | scan status | `FM6000_SCAN_STATUS` |
| `0x1c046` | `PLL_STATUS` | `FM6000_PLL_STAT` |
| `0x1c048` | `SWEEPER` | `FM6000_SWEEPER_CFG` |

It also confirms the two corrections this port made against its own earlier
guesses: `ESCHED_CFG_1/2/3` really are at `0x2000`/`0x2080`/`0x2100`, and every
`SSCHED` address in `regs.h` is right.

### What it gives us that we did not have

- `SSCHED_{RXQ,TXQ,HS,}_FREELIST_INIT` and a `_DONE` beside each, `0x80f0`–`0x80fd`
- `ESCHED_DRR_Q` `0x3000`, `ESCHED_DRR_CFG` `0x3800`, `ESCHED_DRR_DC_INIT` `0x3c00`
- `CM_ESCHED_STATE` `0x116c00` — the first register we have that reports on the
  egress scheduler from *outside* the block that is stuck
- the whole `FC_MRL_*` block, `0x28000`–`0x28022`

### ~~The freelist discrepancy~~ — withdrawn, it was a misreading

This section previously reported that all four freelist `_DONE` registers read
0 on a chip that had completed Table 4-1, and called it the first concrete
asymmetry between what the boot controller claims and what the scheduler shows.

That was wrong. `SSCHED_*_FREELIST_INIT` is a **data-push port** and
`*_INIT_DONE` a **write-1 strobe**: the vendor's freelist loader writes one
entry at a time into `0x80fc` and then writes `1` to `0x80fd`. Neither reads
back, so reading them proves nothing and there was never an asymmetry.

This is the third time this port has read a write-side port as status — the
TXMP tables and `RX/TX_INIT_TOKEN` were the first two. On this chip, a register
that reads 0 after being written is the normal case, not the interesting one.

What *is* established: the BM block is populated after Table 4-1, presumably
from the fusebox — `BM_TXQ_HS_SEGMENTS` `0x3ff7`, `BM_RXQ_PAGES` `0x3fe`,
`BM_MODEL_INFO` `0x002abff7`. Those three are exactly what the vendor's
production `fm6000FreelistPointerInit` writes, so freelist *geometry* is not
the missing piece. (The file-driven freelist loader in the SDK is a debug path
gated on the `api.FM6000.debug.freelist` attribute, not the boot path.)

## The chip has been resetting itself the whole time

Found 2026-09-28, and it reframes most of the port's history.

### The mechanism

`FATAL_CODE` (word `0x6`) has three documented writers: an uncorrectable SRAM
error, **a CRM access timeout**, and a direct write from a bus master. When it
is written, the watchdog copies it to `LAST_FATAL_CODE` (`0x7`), clears it,
increments `FATAL_COUNT` (`0x8`), waits 64 cycles and asserts `MASTER_RESET` —
which puts the management module and the core fabric back to their defaults.
[DS §4.2, "FATAL_CODE Register"]

The chip then comes back on the local bus by itself. **That is why nobody
noticed.** A sequence that was wiped half way through reports every step `ok`
and leaves nothing behind.

The SCD's reset pulse is `CHIP_RESET_N`: it zeroes `FATAL_COUNT` and
`LAST_FATAL_CODE`, so a count is always "since the last pulse". Measured.

### What it has been costing us

From a chip-reset pulse with `FATAL_COUNT` at 0:

| after | `FATAL_COUNT` | `LAST_FATAL_CODE` |
|---|---|---|
| `--boot` | 8 | `0xc5` |
| `--ssched` | 315 | `0xa4` |

Table 4-1 resets the chip eight times while reporting success. The scheduler
ring init resets it **three hundred and fifteen** times. Every conclusion ever
drawn about this ring was drawn on a chip being reset several times a second.

### Where the boot's eight come from

All eight land in step 3 — the step-5 write of `0xFFFFFFFF` to
`SCAN_CHAIN_DATA_IN`. Sweeping all 32 values of `SCAN_CONTROL` (`0x1c039`)
before that write gives a perfectly clean split:

| `SCAN_CONTROL` | self-resets |
|---|---|
| `0`–`15` (bit 4 clear) | **0** |
| `16`–`31` (bit 4 set) | 8 |

32 of 32, no exceptions, and the reset default has bit 4 set — which is why we
get eight without writing the selector at all. What bit 4 *means* is not
established, and "clear it" is not yet a fix: an inert write is not the same as
a correct one.

### Where the ring init's come from: one write

`SWEEPER_CFG` is **one eight-word register** at `0x1c048` (the map gives
`0x1c050` as the next register), so what this port called `SWEEPER_CFG_0..4`
are words of it. It programs the manageability module's reference timers —
PAUSE, POLICERS, the L2 lookup sweepers and FRAME TIMEOUT [DS §9.2], and the
very next section of the datasheet is the Counter Rate Monitor.

Writing each word **alone, from a fresh boot**, and sampling `FATAL_COUNT`
three times after:

| write | `FATAL_COUNT` |
|---|---|
| `TICK_CFG` only (control) | 8, stable |
| `SWEEPER_CFG` word 0 (`0x1c048` ← `0x0008bb2c`) | 8, stable |
| word 1 (`0x1c049` ← `0x2`) | 8, stable |
| word 2 (`0x1c04a` ← `0x0`) | 8, stable |
| **word 3 (`0x1c04b` ← `0x0030a2c3`)** | **storm** |
| **word 4 (`0x1c04c` ← `0x00002000`)** | **storm** |

Two independent triggers, not one. Isolating them needs a fresh boot per word:
a first pass that wrote them in order made word 4 look harmless, because word 3
had already started the storm and there is no way to tell a second trigger from
a continuing one. Arming those timers sets background engines walking the
MAC table and the policer banks; on a chip whose tables are not initialised
those accesses time out, a CRM access timeout writes `FATAL_CODE`, and the
watchdog resets the chip — forever. The value being written is a golden one
captured from a *fully configured* switch, which is exactly the chip state that
makes it safe.

### The result

`fm_ssched_ring_init()` takes `FM_SSCHED_NO_SWEEPER`, and `fm6000-probe
--ssched nosweep` uses it. With it, the ring init completes with **zero**
self-resets and `FATAL_COUNT` stays at 8 indefinitely.

The ring still does not circulate. But it is now being programmed into a stable
chip for the first time, so for the first time the question is answerable.

### Use this

`fm6000-probe --fatal` reports the count, the last code, and which SRAMs have
logged uncorrectable errors. `--boot` and `--ssched` now report self-resets per
step and per phase. **Read the count before and after any sequence on this chip
and treat an increase as failure, whatever the sequence said.**

## `WATCHDOG_CFG` bit 0 freezes the chip instead of fixing it

`WATCHDOG_CFG` (word `0xb`) has exactly one writable bit: bit 0. Writing `0x2`,
`0x4` or `0x8` reads back `0`; writing `0xffffffff` reads back `0x1`.

It does not stop the chip resetting itself. It stops the chip *recovering*.

| bit 0 | arming `SWEEPER_CFG` word 3 |
|---|---|
| clear (default) | `FATAL_COUNT` jumps around forever — the storm |
| set | count stops at exactly `0x16` and holds indefinitely |

Measured twice each way. With it set, only the watchdog block still answers:
`BOOT_CTRL`, `SOFT_RESET` and `PIN_STRAP` all stop reading and writes stop
sticking, which is what "held in `MASTER_RESET`" looks like from outside. The
watchdog survives because `MASTER_RESET` does not reset the watchdog itself.

**Use it as a diagnostic.** When you want to know which fatal code a particular
trigger produces, set bit 0 first: the storm otherwise overwrites
`LAST_FATAL_CODE` faster than it can be read. With it set the code is stable
(`0xa6` for the sweeper trigger, read six times over twelve seconds). Recovery
is a `CHIP_RESET_N` pulse like any other held chip.

## Other things measured on a stable chip

- **The SSCHED block accepts writes.** Write-then-read after boot: `SLOW_PORT`
  (`0x8070`) and the visit table (`0x8000`, `0x8001`) hold what is written.
  `RX/TX_INIT_TOKEN` (`0x8020`, `0x8060`) read back 0 — they are insertion
  ports, not storage, which is what "INIT_TOKEN" should be. So the ring not
  circulating is not the block refusing our writes.
- **Filling the policer banks storms too.** `POLICER_CFG_4K` `0x130000`,
  `POLICER_CFG_1K` `0x134000`, `POLICER_STATE_4K` `0x138000`,
  `POLICER_STATE_1K` `0x13c000`: filling all four took `FATAL_COUNT` from 8 to
  90. The fill reports success because the chip keeps coming back.
- **The CRM is not running**: `CRM_CTRL` (`0x1f000`) and `CRM_STATUS` are 0,
  `CRM_IM` is `0xffffffff` (everything masked), and `CRM_COMMAND` (`0x1f080`)
  reads uninitialised garbage. So "CRM access timeout" is not the CRM's own
  program running amok — it is accesses through the management ring failing to
  complete.
- The tables the sweepers walk, for whoever initialises them next:
  `L2L_MAC_TABLE` `0x280000`, `L2L_MAC_TABLE_SWEEPER` `0x2c0000`, and the four
  policer regions above.

## The CRM initialises memory in hardware, and we were only doing one region

Table 4-1 step 12 offers two ways to initialise memory: "Use CRM to setup
memory table. Launch CRM execution. Wait for completion." Or: "Software writes
memory manually." This port has always done the second. That is wrong twice
over.

**Mechanism.** A software fill is the CPU writing every word through the
management ring, and an access that cannot complete raises a CRM access
timeout and resets the fabric.

**Coverage.** The vendor initialises **128 memory regions** this way before
anything else runs — every parser, mapper, policer, L2AR, MOD, FFU, stats and
MAC table on the die. We initialise exactly one, `STATS`, by hand.

`datapath/fm6000/crm.c` implements the hardware path. The field packing came
from the datasheet's field order and was then confirmed field by field against
the vendor SDK's own `fm6000CrmSetMemory` — `Command[2:0]`, `Count[33:14]`,
`BaseAddress[21:0]`, `Size[23:22]`, the four shift fields at `[27:24]`,
`[31:28]`, `[35:32]`, `[39:36]`, and `CRM_CTRL` as `Run[0]`,
`FirstCommandIndex[6:1]`, `LastCommandIndex[12:7]`. The SDK also computes
`CRM_COMMAND`'s address as `(slot + 0xf840) * 2` = `0x1f080 + slot*2`, which
confirms base and stride independently of the register map.

### It works, and it narrows the problem sharply

Each run from a fresh boot with `FATAL_COUNT` at 8:

| region | width | result |
|---|---|---|
| `STATS` `0x200000` | 32-bit | **8 → 8**, clean |
| `L2L_MAC_TABLE` `0x280000` | 32-bit ×1024 | **8 → 8**, clean |
| `POLICER_CFG_4K` `0x130000` | 64-bit ×4096 | **8 → 8**, clean |
| `POLICER_STATE_4K` `0x138000` | 64-bit ×4096 | storms |
| `MCAST_DEST_TABLE` `0x240000` | 96-bit ×4096 | storms |
| `MCAST_VLAN_TABLE` `0x260000` | 32-bit ×32768 | storms |

So the mechanism is sound — the same policer region that cost 82 self-resets
under a software fill has a sibling that the CRM fills for nothing. What
remains is three specific memories, and the pattern is suggestive: the *config*
banks initialise and the *state* banks do not.

### Register widths are not guessable, and getting them wrong looks identical

The SDK passes a width per region: `L2L_MAC_TABLE` is 4 words per entry,
the policer banks 2, `MCAST_DEST_TABLE` 3 significant of 4 stride,
`MCAST_VLAN_TABLE` 1, `STATS` 2 — and `STATS` is filled with `0xffffffff`,
not zero as this port has been doing.

This matters: filling a 64-bit ECC entry as two independent 32-bit registers
writes half an entry and leaves invalid ECC, which faults exactly like a
memory that was never initialised. A first pass here ran every region as
32-bit and had to be redone.

### Correction: `0x240000` and `0x260000` are memories, not register blocks

This document previously recorded them as "register blocks, not banks" because
a software fill died at `0x240036` and `0x260014`. They are
`FM6000_MCAST_DEST_TABLE` and `FM6000_MCAST_VLAN_TABLE`, and both are in the
vendor's CRM initialisation list. The fill died because a software fill is the
wrong mechanism for them, not because they are not memories.

## Running the vendor's whole memory init

`fm6000-probe --crm-batch FILE` runs a list of `base count size value` lines.
The list is a **runtime input, not part of NOSaic** — it is chip geometry
recovered for bring-up, and the tree does not carry it. Recover it from the
SDK's `MemoryInitCRM` (`api/fm6000/fm6000_api_init.c`, entry `0x3be713` in
EOS 4.16.8M's `libFocalpointSDK.so`), which is the only caller of the CRM
memory-set wrapper and issues exactly 129 of them:

```sh
objdump -d libFocalpointSDK.so | awk '
/movl +\$0x[0-9a-f]+,0x[0-9a-f]+\(%esp\)/ {
  match($0,/\$0x[0-9a-f]+/); v=substr($0,RSTART+1,RLENGTH-1)
  match($0,/,0x[0-9a-f]+\(%esp\)/); o=substr($0,RSTART+1,RLENGTH-7); a[o]=v; next }
/call.*<fm6000CrmSetMemoryExt@plt>/ {
  printf "0x%06x %d %d %s\n", strtonum(a["0x4"]), strtonum(a["0x10"]),
                              strtonum(a["0xc"])-1, a["0x8"]; delete a }'
```

The arguments are `(sw, base, value, words, count, ...)`, and the CRM `Size`
field is `words - 1` — 1, 2, 3, 4 words per entry mapping to 32, 64, 96 and
128-bit registers.

### What it achieves

About a hundred of the 129 regions initialise with **zero** self-resets. The
rest hard-reset the chip, and the batch stops at the first one because the chip
does not come back on its own — so a run reports the first failure, not all of
them. Re-running with that region excluded simply finds the next.

Confirmed region-specific rather than positional: `MOD_L2_VLAN1_TX_TAGGED`
`0x150000` and `MOD_CAM` `0x158000` fail on their own from a fresh boot, while
`FFU_SLICE_CAM` `0x380000` — later in the list — is clean.

Known failures so far: `POLICER_STATE_4K` `0x138000`, `POLICER_STATE_1K`
`0x13c000`, `CM_QUEUE_STATE_INIT` `0x118800`, `MCAST_DEST_TABLE` `0x240000`,
`MCAST_VLAN_TABLE` `0x260000`, `ESCHED_DRR_DC_INIT` `0x003c00`,
`MOD_L2_VLAN1_TX_TAGGED` `0x150000`, `MOD_CAM` `0x158000`.

`ESCHED_DRR_DC_INIT` is the chicken-and-egg we already know about: the egress
scheduler is unreachable until the ring circulates, and the ring is what we are
trying to start.

### The ordering question, answered

The SDK calls `MemoryInitCRM` once, from its top-level init, **before**
`fm6000InitRegisterCache`. The two register writes immediately preceding it are
`FC_MRL_RATE_LIMITER` `0x28022` and `FC_MRL_FC_TOKEN_LIMIT` `0x28020`. So the
memory init comes early and the sweepers are armed over already-initialised
tables — which is the opposite of the order this port has been using.

## The definitive result: 114 of 129 memories initialise, and the 15 that do not are all egress

Every one of the 129 regions was run **alone, from a fresh boot**, with
`FATAL_COUNT` checked after each — 129 boots, so that a region is judged on its
own and not on the wreckage of the one before. 114 clean, 15 failing.

The 15 that hard-reset the chip:

| address | register |
|---|---|
| `0x003c00` | `ESCHED_DRR_DC_INIT` |
| `0x118800` | `CM_QUEUE_STATE_INIT` (listed twice by the vendor) |
| `0x138000` | `POLICER_STATE_4K` |
| `0x13c000` | `POLICER_STATE_1K` |
| `0x150000` | `MOD_L2_VLAN1_TX_TAGGED` |
| `0x154000` | `MOD_L2_VLAN2_TX_TAGGED` |
| `0x158000` | `MOD_CAM` |
| `0x15a000` | `MOD_MAP_IDX12A` |
| `0x15b000`–`0x15e000` | `MOD_MAP_DATA_W16A`–`W16D` |
| `0x240000` | `MCAST_DEST_TABLE` |
| `0x260000` | `MCAST_VLAN_TABLE` |

That list is not arbitrary. **Every one of them is on the egress side**: the
modification block, the multicast tables, the congestion manager's queue state,
the policer state, and the egress scheduler's own DRR initialiser. Everything
ingress — parser, mapper, FFU, L2AR, L3AR, the MAC table, the policer *config*
banks, stats — initialises without a murmur.

### What it buys, and what it does not

Running the 114 as one batch: **114 initialised, 0 failed, `FATAL_COUNT`
unchanged**. That is a complete, clean memory initialisation of everything
reachable, on a chip that never resets itself once. It is a far better starting
point than this port has ever had.

It does not start the ring. `--ssched nosweep` still reports programmed but not
advancing.

And it does not make the sweeper safe: with all 114 initialised, arming
`SWEEPER_CFG` word 3 still storms. That is consistent rather than
disappointing — the policer sweeper walks `POLICER_STATE`, and `POLICER_STATE`
is one of the fifteen we cannot initialise. The chain closes on itself.

### So the question is now one question

Why do fifteen egress-path memories reject even a hardware CRM walk, when a
hundred and fourteen others accept one? Every remaining symptom — the ring not
circulating, the sweeper storming, `ESCHED` unreadable, `CM_ESCHED_STATE` at
zero — hangs off that. It is a much smaller and much better-posed question than
the one this started with.

## The BIST controllers were never configured — and it still is not the answer

`fm6000BistMemoryInit` in the SDK sets up **fourteen** BIST controllers. This
port drove **two**: `BM_MARCH` and `SRBM_MARCH`.

Measured on a chip that had completed Table 4-1 end to end, all five controller
instances (`0x1d400` at stride `0x80`) read `MAX_ADDR` 0, `START_SEQUENCE` 0
and `CHAIN_GENERAL_CONFIG` 0. The documented boot does not configure them and
neither did we.

The structure, from the register map's `SPDP_BIST` naming repeated per
instance: `+0x09` `MAX_ADDR`, `+0x0b` `START_SEQUENCE`, `+0x40`
`CHAIN_GENERAL_CONFIG`, `+0x41` `CHAIN_LATENCY`. Five instances with address
ceilings `0xfff`, `0x7fff`, `0x3fff`, `0xfff`, `0x3ff` and sequence selectors
`0`, `2`, `2`, `2`, `0`; eight chain instances at stride `0x20` with latencies
`4`,`4`,`4`,`4`,`6`,`6`,`0xa`,`0xa`; five CDP chains taking the general config
only. `fm_bist_configure_controllers()` does it, 34 writes.

**It is a real missing step and it is safe**: 34 writes, `FATAL_COUNT`
unchanged, chip answering. The march then completes in 29 ms.

**It does not unlock the fifteen.** `POLICER_STATE_4K` still hard-resets the
chip under a CRM walk after both the controller configuration and the march.
So a missing BIST setup is not why those memories are unreachable — though the
configuration is worth keeping regardless, since running a march on
unconfigured controllers was never right.

One loose end: the march reports **one** result register set where zero is
wanted. A real defect, or a controller still not configured the way this part
expects.

### Hypotheses eliminated for the fifteen

- **`SOFT_RESET`** — all five bits are named and boot leaves it at 0.
- **Freelist sizing** — the BM block is populated after Table 4-1
  (`BM_TXQ_HS_SEGMENTS` `0x3ff7`, `BM_RXQ_PAGES` `0x3fe`, `BM_MODEL_INFO`
  `0x002abff7`), which is exactly what the vendor's `fm6000FreelistPointerInit`
  writes.
- **Freelists reading empty** — a misreading; those are write-side ports.
- **Memory-init order** — 114 regions initialise cleanly first and it changes
  nothing.
- **BIST controller configuration and march** — now done, and it changes
  nothing.

## Our ring init now matches the vendor's, and it still does not circulate

The vendor's ring builder sits immediately before `fm6000ValidateSchedulerToken`
in the SDK. Compared line for line with `ssched.c`:

| step | vendor | us |
|---|---|---|
| ring tokens | `0x8060`, `0x8020` | same |
| visit table | `0x8040+i` and `0x8000+i`, i = 0..19 | same |
| slow-port mask | `0x8070+i`, **i = 0..4**, 16-bit values | **was one word** — fixed |
| `INIT_COMPLETE` | `0x8061`, `0x8021` ← 1 | same |

Two things came out of that comparison.

**The slow-port mask was wrong.** `ssched.c` wrote one word and carried a
comment claiming "the running switch writes only the first, and the other four
are left as the boot leaves them". The vendor loops over all five, and each
takes a *sixteen*-bit value — so this is one bit per scheduler port across 80
bits, not a 32-bit mask in a single register, which matches the 76-port segment
scheduler. Now written in full, with the upper four explicitly zero rather than
left to whatever the boot put there.

**Our circulation check is exactly the vendor's validation.**
`fm6000ValidateSchedulerToken` writes `0x8062`, delays `0xc350` (50 ms), reads
it back, and repeats for `0x8022` — the same registers and the same 50 ms our
find-probe uses. So the probe is right and the ring genuinely is not advancing.

Neither fix starts the ring.

### The reframing that matters

The SDK calls `ValidateSchedulerToken` **before** `MemoryInitCRM`. The vendor
expects the ring to be circulating on a chip whose 129 memories have not been
initialised yet.

So the ring does not depend on the memories — which is exactly what we measured
from the other direction when initialising 114 of them changed nothing. And it
means the ring should come up very early, on a chip that has had little more
than Table 4-1.

Since the builder is now faithful, the difference is upstream of it.

## Step 5 needs the scan engine stopped first — 8 self-resets down to 3

The datasheet describes Table 4-1 step 5 as a single write of `0xFFFFFFFF` to
`SCAN_CHAIN_DATA_IN`, and this port did exactly that. It cost **eight** watchdog
self-resets, every time — all eight of the boot's total.

The vendor's `fm6000PrebootSwitch` writes three words to
`SCAN_CONFIG_DATA_IN` immediately before that write:

```
0x1c03a <- 0x88800000
0x1c03a <- 0x88008000
0x1c03a <- 0x80000040      opcode 0x80 operand 0x40 -- the scan engine's stop
0x1c03b <- 0xffffffff      then step 5
```

Doing the same takes the boot from **8 self-resets to 3**. Isolated from a bare
reset pulse: the three config writes are free, and the chain write alone
accounts for the remaining three.

Reading that as "reset leaves the scan engine running and writing the chain
underneath it faults" is inference. What is measured is the count either side.

### Correction: the MRL fix runs BEFORE step 5, not after

This document and `mrl.h` both said `fm6000MrlRegisterFix` was applied after
the boot commands. That is wrong. `fm6000PrebootSwitch` calls, in order:

1. `fm6000BistMemoryInit` — the per-memory BIST controller setup
2. `fm6000MrlRegisterFix`, or `MrlRegisterFixVersion2`, chosen by an API
   attribute
3. the three `SCAN_CONFIG_DATA_IN` quiesce words
4. Table 4-1 step 5's chain write

So both the BIST setup and the scan program belong in the pre-boot, ahead of
step 5 — and this port does neither there. The remaining three self-resets are
the obvious place that shows.

Tested: running the 34 BIST controller writes before the quiesce and the chain
write, from a bare reset pulse, leaves the count at three. So it is not the
BIST half. By elimination the remaining three belong to the MRL scan program,
which is the one pre-boot step we cannot reproduce — it needs the payload this
tree deliberately does not carry.

That is a hypothesis by elimination, not a measurement, and it is worth saying
plainly: if it is right, the last three self-resets in Table 4-1 are not
fixable without the vendor's chain data, and the useful question becomes
whether three resets that early actually matter, given the boot's later steps
all run after them.

## Step 5 has never taken effect, and now it does

The three remaining self-resets all land on step 5's chain write, and the
watchdog puts the fabric back to defaults under it — so whatever step 5
configured is gone before step 6 begins. "Core logic and EPLs to normal
operating mode" is a step this chip has never actually had.

The same write on a chip that has finished the sequence is **free**: measured
twice, with and without the quiesce in front of it, `FATAL_COUNT` does not
move. So `boot.c` now does step 5 twice — once in its documented position, and
again once the chip has settled, which is the only place measured to work.

Why it is free later is not established. The obvious guess is that the modules
being out of soft reset is what lets the write retire, which would mean Table
4-1's ordering does not hold on this part. That is a guess; the measurement is
the reset count.

It does not start the ring.

## The unified hypothesis: the bank scan chain is what we are missing

Three findings from this session fit together, and the fit is worth stating
even though it is inference.

1. The MRL scan program shifts **5800 words into scan chain `0x14`** and 203
   into chain `0x10`. Chain `0x14` is the one this port named `BANKS` — the
   program's bulk payload goes into a chain that configures memories.
2. **Fifteen memories reject even a hardware CRM walk**, and all fifteen are
   egress-path. Nothing else about them explains it: not `SOFT_RESET`, not
   freelist sizing, not memory-init order, not BIST configuration or march.
3. **`ESCHED_DRR_DC_INIT` is one of the fifteen.** The egress scheduler's own
   initialiser is among the memories we cannot reach — and the ring will not
   circulate.

Read together: the bank scan chain configures those memories, the MRL program
is what loads it, and without it the egress memories stay unreachable — one of
which is the scheduler's own. That would make the ring not a scheduler problem
at all, but the most visible symptom of the memories behind it.

### What makes it hard to dismiss

`fm6000PrebootSwitch` always runs one of the two scan programs. The API
attribute at `0x3c83a8` chooses between `fm6000MrlRegisterFix` (6287 entries)
and `fm6000MrlRegisterFixVersion2` (**12532** entries, its own table) — it does
not choose whether. There is no supported vendor configuration in which the
chip boots without a multi-thousand-word scan program running before step 5.

### What would settle it

Nothing we can run today. The zero-payload sequence is destructive after the
boot, and in the vendor's pre-boot position it is still zeros — it cannot put
the chain into the state real data would.

The honest position: **the boot as the vendor performs it is not reproducible
without the chain data**, and the ring may be downstream of exactly that. The
route that stays open is the derivability question parked earlier — of 6003
payload words only 185 are non-zero, every one a five-bit field at bit 15, in
four clusters each periodic with period 25, two of which are rotations of the
other two. That is on the order of a hundred numbers, and if their meaning can
be worked out they can be generated rather than copied.

## What the vendor writes during bring-up that this port never does

Produced by extracting every register address the SDK writes as an immediate
(367 of them) and subtracting every address named anywhere in
`datapath/fm6000`. The raw difference is 251 entries, but most are forwarding
tables we have no feature for yet, and some are false positives — the
extraction catches the first argument of any call, so constants like `0xc350`
(the 50 ms delay) and `0xf4240` appear as if they were addresses. What follows
is the filtered, credible remainder.

**Interrupt and status plumbing** — none of it needed to forward, all of it
needed before anything is trustworthy:
`INTERRUPT_MASK_PCIE` `0x1c002`, `GLOBAL_EPL_INT_DETECT` `0x1c004`,
`SW_IP`/`SW_IM` `0x1c01c`/`0x1c01d`, `SW_TEST_AND_SET` `0x1c01e`,
`CM_INTERRUPT_DETECT` `0x22100`, `SRBM_IP` `0x1d70e`.

**Congestion and rate limiting**: `CM_GLOBAL_USAGE` `0x110200`,
`CM_PAUSE_PACING_CFG` `0x116600`, `CM_PORT_TXMP_IP_WM` `0x20800`,
`CM_PORT_TXMP_SAMPLING_PERIOD` `0x21000`, `ERL_CFG` `0x117000`,
`ERL_CFG_IFG` `0x117800`, `FRAME_TIME_OUT` `0x1c01f`.

**The L2 lookup sweeper block**, `0xd000`–`0xd408` — timer config, CAM, FIFO
and its head/tail, write command and data. Untouched here, and it is one of
the engines `SWEEPER_CFG` arms.

**The MAC table's configuration**, `L2L_MAC_TABLE_CFG` `0x30000`, plus the
VID/lock tables.

**Buffer manager sizing**: `BM_TXQ_HS_SEGMENTS` `0x1d085`, `BM_RXQ_PAGES`
`0x1d086`, `BM_MODEL_INFO` `0x1d087`, `BM_VRM` `0x1d089`. The vendor writes
these; on our chip they are already populated from the fusebox, so this is a
difference in method rather than in outcome.

**Test and debug control**: `RO_CFG` `0x1c052`, `TESTCTRL_*` `0x1c054`–`0x1c058`.

**The PCIe block** `0x1400`–`0x1435`, which is packet DMA and belongs to M5.

### And one hypothesis this killed

`PLL_CTRL` `0x1c042` is in that list, and Table 4-1 step 6 says "initialize PLL
and wait for lock" while `boot.c` only waits. A fabric running on the wrong
clock would explain the ring, the timeouts and the unreachable memories all at
once, so it was worth checking.

It is not that. `PLL_CTRL` reads `0x20841436` on a chip that has had nothing
but a reset pulse — populated from the fusebox — and `PLL_STAT` goes from `3`
to `0xf` across our boot, all four lock bits. `DLL_CTRL` is likewise
pre-populated at `0x08011b05`. The clock is fine.

## The two MRL tables, compared

The SDK ships two scan programs and `fm6000PrebootSwitch` picks between them by
API attribute. Comparing them is the closest thing to a controlled experiment
available without hardware:

| | entries | chain `0x14` words | CFG words | non-zero payload |
|---|---|---|---|---|
| `fm6000MrlRegisterFix` | 6287 | 5800 | 281 | 449 |
| `...Version2` | 12533 | 11600 | 521 | 901 |

Version2 is almost exactly double, and over their common 6287 entries **only 32
differ — half a percent**. They are the same program with a handful of
per-variant values, the first divergence being one five-bit field holding 11 in
one and 3 in the other.

That matters for the derivability question: the program is overwhelmingly fixed
structure, not bulk per-die data. It does not tell us what the values *mean*,
which is still what stands between us and generating them.

## The sweeper storm is gone: derive the value, do not capture it

Two things came together here.

**`fmPlatformSwitchPreInitialize` does not write a constant.** Gated on the
`api.fm6000.mrlPatch` attribute, which defaults on, it read-modify-writes:

```
v = read(0x1c04b);  v = (v & 0xffff) | 0x300000;        write(0x1c04b, v)
v = read(0x28020);  v = (v & ~0x3ff) | 0x190 | 1<<30;   write(0x28020, v)
```

`ssched.c` wrote a captured `0x0030a2c3` to the first of those. On a chip where
`0x1c04b` already held `0xa2c3` the vendor's formula produces exactly that — so
the captured value was another chip's *result*, not an input. Ours reads **0**
after Table 4-1, so the correct value here is `0x300000`. The second register,
`FC_MRL_FC_TOKEN_LIMIT`, this port never wrote at all.

**And the step-5 fix cured word 3 independently.** Before it, writing anything
to `SWEEPER_CFG` word 3 started a permanent storm. After it, both the captured
value and the derived one are free. So the storm was never really about the
value — it was a chip whose step 5 had been reset out from under it.

### Result

The full ring init, with the sweeper armed and no `nosweep`, now runs with
**zero self-resets**. That path cost 315 in one measurement and 507 in another.
`FATAL_COUNT` stays at 3 — the three the boot still spends on step 5 — and the
registers land as the vendor computes them: `SWEEPER_CFG` word 3 at `0x300000`,
`FC_MRL_FC_TOKEN_LIMIT` at `0x40000190`.

The ring still does not circulate.

### Word 4 is left at zero, deliberately

The captured `0x00002000` for `SWEEPER_CFG` word 4 still storms, measured alone
from a fresh boot, and writing 0 is free. Bit 13 most likely arms the L2 lookup
sweeper — but CRM-initialising the MAC table first does not make it safe, so
what it wants is not known. A register whose value we cannot justify is better
left alone than written from a capture. That is exactly what word 3 was.

### And a correction to the unified hypothesis

The previous section argues the ring may be blocked on the MRL chain data. That
is weaker than it was written. The attribute selecting between the two scan
programs is `api.FM6000.enableBemPerfTuning` — a **performance tuning** switch,
which is not the shape of something a scheduler cannot run without. The
hypothesis is not dead, but it should not be leaned on.

## SWEEPER_CFG is one register with fields, not five words of constants

`fm6000ApplyPolicerSweeperCfg` reads five words from `0x1c048`, sets a
bitfield, and writes them back — `fmMultiWordBitfieldSet32(buf, 0x6f, 0x60, v)`,
bits **[111:96]**, which is word 3 bits `[15:0]`. Being `ApplyPolicerSweeperCfg`,
that field is **PolicerPeriod** — and it is exactly the low sixteen bits the
`mrlPatch` preserves.

Across the whole SDK only two fields of this register are ever set: word 0 in
full, and word 3's PolicerPeriod. Words 1, 2 and 4 are never written as
immediates at all. So the right model is read-modify-write of named fields, not
five captured constants, which is what this port had.

### PolicerPeriod is not why word 4 storms

Tested, since a period of zero would mean a sweeper running flat out:

| word 3 | word 4 | result |
|---|---|---|
| `0x00300000` (period 0) | `0x00002000` | storms |
| `0x0030a2c3` (period `0xa2c3`) | `0x00002000` | **still storms** |
| `0x0030a2c3` | `0` | clean |

So a sane period does not make word 4 safe, and the period hypothesis is
dead. The SDK never sets bit 141 either. Word 4 stays at zero.

## The ring builder writes seven registers and nothing else

The vendor's builder — the whole function, not just the part compared earlier —
writes only: `0x8060`, `0x8020`, the `0x8040`/`0x8000` visit loop, the `0x8070`
slow-port loop, then `0x8061`/`0x8021`. No tick, no sweeper. Those live in the
pre-boot and platform init instead. `ssched.c` bundles them into the ring init,
which is harmless but is not the vendor's structure.

### RX and TX tokens are not identical

The RX token takes the port in `[6:0]` and one flag into bit 9. The TX token
takes the same two and then computes **bit 10** from a second per-entry field
the RX path never reads.

`ssched.c` writes one token to both directions. On the golden capture every
token has bit 10 clear, so here the two coincide and it is not wrong — but it
is an assumption the code was making silently, and it holds only while that
field is zero. Now commented.
