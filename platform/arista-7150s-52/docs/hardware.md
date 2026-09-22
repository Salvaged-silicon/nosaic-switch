# Arista DCS-7150S-52 — hardware reference

The deep page: how this switch is built and how NOSaic will drive it. The
audience is somebody writing the FM6000 datapath or debugging silicon.

**Provenance matters more on this page than on any other board's.** NOSaic does
not run on this switch yet, so nothing below has been measured *by our code*.
Each table says where its numbers come from:

| Mark | Means |
|---|---|
| **live** | read off the running unit (`sw7150-lab`, EOS 4.16.8M) with `lspci`, `/proc/cmdline` or a BAR0 probe |
| **derived** | worked out from the investigation and believed, but not confirmed from a second direction |
| **assumed** | stated so it can be checked; not evidence |

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
              AMD Family-10h (x86_64, ~4 GiB)
                        |
        +---------------+----------------+
        |                                |
   RS780 northbridge                SB700/SB800 south
        |                                |
   PCIe port 0 (00:04.0)           00:14.6  BCM5785 GbE  -> ma1 (tg3)
        |                           00:11.0 SATA -> USB DOM /dev/sda
   02:00.0  FM6000 "Alta"           00:12.x/13.x USB
            8086:155b               SMBus 1002:4385
            BAR0, packet DMA
            at BAR0+0x5000
                                   PCIe port 5 (00:05.0)
                                        |
                                   04:00.0  SCD FPGA  3475:0001
                                            BAR0 0xe1000000, 256 KiB
                                            holds the FM6000 in reset
```
*(live — `lspci -nn` on the running unit.)*

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

## The first thing that will confuse you

**The ASIC is held in reset by the SCD from power-on.** A bare kernel boots,
`lspci` shows no `02:00.0`, and nothing anywhere says why. That is the normal
state of this board, not a fault — the SCD's reset GPO (`0x4000` region) has to
be driven before the chip appears. *(derived.)*

The second thing: **this box does not reboot.** EOS does not use the hardware
reset here at all; its halt script tries `kexec` and only falls back to a reset
it does not trust. Every hardware reset path tried from a bare kernel on this
chassis has hung. Until that is solved, recovery is the power controller.
*(live, for EOS's behaviour; derived for ours.)*

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
| BAR0+`0x5000` | packet DMA | TX/RX descriptor rings (byte offset, not word) |

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

So: **no vendor blob, at any point.** NOSaic writes a parser microcode generator
and emits the Action SRAM itself. What it parses is then our decision rather
than Arista's — and a switch that needs Ethernet, VLAN, IPv4, IPv6 and TCP/UDP
needs a good deal less parser than a vendor image that supports everything.

The generator is Apache-2.0 code in this repository like anything else.
Knowledge taken from a datasheet is not vendor code: the datasheet itself is
Intel's and is fetched rather than committed, the same rule
[docs/datasheets.md](../../../docs/datasheets.md) applies to every other board.

What is not yet known, and has to come off the chip: **where the Action SRAM
lives** in the register map, how many slices this part has, and how a slice's
SRAM is indexed by state. The encoding is documented; its address is not.

## Front panel

52 SFP+ cages, 10G, numbered 1..52 on the silkscreen. No QSFP, and no external
PHYs — every port is direct serdes off the FM6000, so there is none of the PHY
firmware loading the 7050TX-64 and AS4610 need.

**The port map does not exist yet.** Front-panel number to EPL instance to
serdes lane is the first table this board needs and the first thing the datapath
will be wrong about. `platform/*/tools/mkportmap.sh` on the Broadcom boards
builds theirs from a `config.bcm`; there is no equivalent file here, so this one
is measured — light one cage at a time and see which EPL's `PORT_STATUS`
changes.

## Transceivers, LEDs, sensors

All behind the SCD, as on the sibling Arista boards: LEDs from `0x5010` (clearing
bit 6 of `0x5010` is what turns an SFP laser on), reset at `0x4000`, interrupts
at `0x3000`/`0x3030`/`0x3060`. The cage EEPROM and sensor layout for *this*
board — which accelerator, which bus, which address — is not known, and the
sibling boards' layout is not transferable. *(derived for the LED and reset
regions; unknown for the rest.)*

## What NOSaic actually drives today

Nothing. This section stays empty until it does not, and the honest form of
this page is one that says so.
