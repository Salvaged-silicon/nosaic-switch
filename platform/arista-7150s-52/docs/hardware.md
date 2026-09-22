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

The single most confusing thing about this board, drawn out:

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

*(live for EOS's behaviour; derived for ours.)* Plan every bring-up iteration
around a four-minute cold cycle, and treat "it will reboot into the other slot"
as a claim to be demonstrated rather than assumed — A/B rollback rests on it.

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

```
   ┌────────────────────────────────────────────────────── ARISTA 7150S-52 ──┐
   │  1  3  5  7 ...                                             ... 49  51  │
   │ ┌─┐┌─┐┌─┐┌─┐                                                 ┌─┐┌─┐     │
   │ └─┘└─┘└─┘└─┘   52 × SFP+  10G   (top row odd, bottom even)   └─┘└─┘     │
   │ ┌─┐┌─┐┌─┐┌─┐                                                 ┌─┐┌─┐     │
   │  2  4  6  8 ...                                             ... 50  52  │
   └──────────────────────────────────────────────────────────────────────────┘
        no QSFP · no external PHYs · every port direct serdes off the FM6000

   front panel N  ──?──▶  EPL instance  ──?──▶  serdes lane
                    ▲                     ▲
                    └── THIS MAPPING DOES NOT EXIST YET ──┘
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
