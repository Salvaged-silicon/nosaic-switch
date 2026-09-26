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
1     SFP+  module present, laser on  0x00000180
2     SFP+  module present, laser on  0x00000180
3..52 SFP+  undetermined              0x00000187

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

## The SBus, from the datasheet and the bench — in progress, 2026-09-25

§9.4 documents the SerDes serial bus, which is the route to every lane. This
section is **not finished**: the addressing is established and a working
transaction is not.

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

⚠ **No successful transaction yet, and this is the open end.** Every attempt
leaves `SBUS_RESPONSE` at zero. `SBUS_CFG` holds only bit 0 on this chip —
writing 2, 3, 4 or 5 reads back 0 or 1 — so the clock-ratio field the
datasheet says "should be set to 4" is **not** the low bits of that register,
and where it is has not been found. That is the next thing to establish, and
until it is, nothing here should be read as a working SerDes path.

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

`fm_rd`/`fm_wr` refuse the bank ranges and ESCHED `0x2000` outright until
`fm_bank_mark_initialised()` has been called, and only the code that genuinely
initialises them is entitled to call it. The instinct when a chip misbehaves is
to go and read more registers; on this part that is what kills it.
