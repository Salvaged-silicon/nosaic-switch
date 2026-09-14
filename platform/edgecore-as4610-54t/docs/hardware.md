# Edgecore AS4610-54T — hardware reference

The deep page: how this switch is built and how NOSaic drives it. The audience
is somebody changing the datapath or debugging silicon.

**Read the provenance note at the bottom before trusting anything here.** Some
of this is measured on a running machine, some is read out of upstream source,
and one section is design rather than observation. They are marked.

## At a glance

| | |
|---|---|
| Model | Edgecore AS4610-54T (Accton `as4610_54`) |
| ONIE platform | `arm-accton-as4610-54-r0` |
| Switch ASIC | Broadcom **BCM56340**, Helix4 |
| Host CPU | 2 × ARM Cortex-A9 r3p0, ARMv7-A, **on the ASIC die** |
| CPU features | `half thumb fastmult vfp edsp vfpv3 tls vfpd32` — **no NEON, no hardware divide** |
| DRAM | 768 MB, based at `0x61000000` |
| Disk | 29 GB behind EHCI at `0x1802a000`, presenting as `/dev/sda` |
| NOR flash | 8 MB on QSPI: U-Boot, shmoo, U-Boot env, ONIE |
| Front panel | 48 × 1000BASE-T (BCM54282 octal PHYs) + 6 × SFP+ (4 uplink, 2 stacking) |
| Management | AMAC at `0x18022000`, SGMII, MAC from U-Boot's `ethaddr` |
| Board controller | CPLD at i2c-0 `0x30` |
| Temperature | one LM77 at i2c-9 `0x48` |
| Console | ttyS0 @ 115200 (`serial@18000400`) |

## The board

```
   ┌──────────────────────────────────────────────────────────────────────┐
   │                    BCM56340  "Helix4"   — ONE DIE                    │
   │                                                                      │
   │   ┌───────────────────────────┐        ┌──────────────────────────┐  │
   │   │      iProc host block     │        │   forwarding pipeline    │  │
   │   │                           │        │                          │  │
   │   │   Cortex-A9   Cortex-A9   │        │   VLAN · L2 · L3 · ACL   │  │
   │   │        └──────┬──────┘    │        │            │             │  │
   │   │          PL310 L2         │        │            ▼             │  │
   │   │               │           │  CMIC  │      MAC / SerDes        │  │
   │   │      AXI  0x18000000 ─────┼────────┤   0x48000000, 256 KiB    │  │
   │   │       │  │  │  │  │       │  SPI   │      + IRQ 162           │  │
   │   └───────┼──┼──┼──┼──┼───────┘        └────────────┬─────────────┘  │
   │        UART│ │  │  │  └─ QSPI ── NOR 8 MB           │                │
   │            │ │  │  └──── i2c-1 ─┐    (U-Boot, ONIE) │                │
   │            │ │  └───────  i2c-0 │                   │                │
   │            │ └──── AMAC ─────┐  │                   │                │
   │            │      EHCI ───┐  │  │                   │                │
   └────────────┼──────────────┼──┼──┼───────────────────┼────────────────┘
                │              │  │  │                   │
             ttyS0         /dev/sda │  │        ┌─────────┴──────────┐
            115200         29 GB    │  │        │                    │
                                    │  │   BCM54282 × 6        BCM84758
                        SGMII PHY ──┘  │   (octal 1G copper)   (10G)
                             │         │        │                    │
                          ma1/mgmt     │    ge0..ge47          xe0..xe5
                                       │    48 × RJ45          6 × SFP+
                                       │
                          ┌────────────┴─────────────┐
                          │ CPLD 0x30   PCA9548 0x70 │
                          │ fans, PSU status,        │
                          │ PHY resets    → 6 × SFP  │
                          │               → PSU × 2  │
                          │               → LM77     │
                          └──────────────────────────┘
```

## What is actually different about this board

Everything above the dashed line in every other NOSaic board's diagram is a
separate chip. Here it is the same piece of silicon, and three consequences
follow that the datapath has to be written around.

**There is no PCI bus, so there is no BAR to map.** `datapath/td2p/bde.c` and
`datapath/tdp/bde.c` both start by finding a PCI device, reading its
configuration space, and mapping `resource0`. None of that exists here. The
chip's register window is a fixed physical range — `0x48000000`, 256 KiB —
declared in the device tree as a platform device.

**So there is no bus-mastering bit, and that is good news.** The AS5610's worst
bug was a P2020 PCIe root port whose Bus Master Enable was clear, so every DMA
the chip issued was correct and was discarded one hop upstream. There is no hop
here. Whatever goes wrong with DMA on this board, it will not be that.

**And there is an interrupt.** The CMIC node carries SPI 162. The AS5610 has no
interrupt delivery to userspace at all and sets `polled_irq_mode=1` so the SDK
polls its own interrupt status register instead. This board should not need to
— but "should not" is doing real work in that sentence, and `config/asic.conf`
deliberately states nothing about interrupts until somebody has watched one
arrive.

### How NOSaic reaches the register window

The datapath is `datapath/helix4/` and the BDE is `bde.c`. **It compiles and
has never run**, so read what follows as the design it implements rather than
as behaviour anybody has seen.

```
   nosd-helix4  (userspace)
        │
        │  map0 → mmap the 256 KiB CMIC window
        │  map1 → mmap the reserved DMA pool
        │  read() → blocks until the chip raises its interrupt
        ▼
   uio_pdrv_genirq              ← mainline, bound by a kernel command-line
        │                         argument, NOT by a compatible string
        ▼
   iproc_cmicd@48000000         ← platform device, from the device tree
```

**The binding is not in the device tree, and that is the part that catches
people.** The obvious way to ask for UIO is a `generic-uio` compatible, and
mainline does not have one: `uio_pdrv_genirq`'s `of_match_table` is a single
empty entry filled in at runtime from its `of_id` module parameter
(`drivers/uio/uio_pdrv_genirq.c`). A node claiming `generic-uio` binds nothing
at all, silently, and the device tree looks perfectly correct while `/dev/uio*`
does not exist.

So the board carries it as a kernel parameter instead:

```
uio_pdrv_genirq.of_id=brcm,iproc-cmicd
```

`nosd-helix4` says exactly this when it cannot find the device, because the
symptom otherwise gives no hint where to look.

Two things make UIO the right shape here rather than a workaround. It is the
same division of labour NOSaic already uses on PCI boards — unmodified
Broadcom code above, a small piece of ours underneath — and it keeps the kernel
out of the datapath entirely, which is what lets the daemon be restarted
without a driver reload.

#### The DMA pool, and why it is a second `reg`

`uio_pdrv_genirq` publishes every memory resource of the platform device as a
UIO map. So the CMIC node carries two `reg` ranges: the register window, and
the 64 MiB region reserved at `0x88000000`. The daemon mmaps map1 and has
physically contiguous memory at an address it knows, with no `/dev/mem` and
nothing for `CONFIG_STRICT_DEVMEM` to refuse.

The reserved region still needs `no-map`, which is what keeps the kernel from
allocating out of it — and is also what makes the fallback work. `bde.c` tries
map1 first and then the AS5610's path, `reserved-memory` plus `/dev/mem`,
because that one has actually run on hardware and this one has not.

**Why not CMA, when this architecture has it.** ARM selects
`HAVE_DMA_CONTIGUOUS`, so `dma_alloc_coherent` works here where it does not on
the AS5610's PowerPC. But it works *for a kernel driver*, and there is no
kernel driver. UIO hands a process a mapping of a physical range and nothing
else, so the range has to exist before anyone asks for it.

#### Two things the SDK wants that this board cannot simply give it

**PCI configuration space.** `soc_cm_device_vectors_t` has `pci_conf_read` and
`pci_conf_write`, and this build calls them. There is no bus, so `sdk.c`
answers from a synthesised structure: vendor and device from the chip's own id
register, a class code, and a COMMAND word with bus mastering set — which is
the truth here rather than a lie to satisfy a check, because there is no bridge
between the chip and memory for that bit to gate. The AS5610's worst bug, a
PCIe root port with Bus Master Enable clear, is structurally absent.

**iProc address space.** There are also `iproc_read` and `iproc_write` vectors,
and they take an absolute address in the SoC's own space — the `0x18000000`
region, where the kernel's i2c, UART and USB drivers live. Mapping all of that
into the datapath would let a bug there write a register the kernel owns, on
the bus the disk is behind.

So nothing is mapped, every access is refused, and the address is recorded and
reported at the end of bring-up:

```
iproc        3 unmapped registers the SDK wanted: 0x1803fc00 0x18032000 ...
             map exactly these by adding a reg range to the CMIC node
```

That is deliberate, and it is the fastest way to learn what this chip actually
needs: one boot produces the list. Deriving it from vendor source would take
longer and be less certain.

## Packet flow

The left-hand side is what `datapath/helix4/` implements and what has never
run. EdgeNOS's is drawn beside it because the two are different and the
difference is easy to miss.

```
                     NOSaic (planned)                    EdgeNOS (today)
                     ────────────────                    ───────────────

  wire ─▶ PHY ─▶ MAC ─▶ ingress pipeline          same silicon, same pipeline
                             │
                  ┌──────────┴──────────┐
                  ▼                     ▼
          HARDWARE FORWARD          PUNT TO CPU
          (never sees the CPU)          │
                                        ▼
                                   CMIC DMA rings
                                        │
                    ┌───────────────────┴───────────────────┐
                    ▼                                       ▼
            nosd-helix4 owns the rings              linux-bcm-knet owns them
            in userspace, over UIO                  (a vendor kernel module)
                    │                                       │
                    ▼                                       ▼
              tap devices                              geN/xeN netdevs
              swp1..swp54                              created by the module
                    │                                       │
                    └──────────────┬────────────────────────┘
                                   ▼
                          Linux IP stack, FRR
```

Both put the chip's punted frames in front of the kernel's IP stack. NOSaic
does it with tap devices fed by a userspace poller
(`datapath/common/tapbridge.c`), EdgeNOS with Broadcom's KNET module. The
consequence for anyone porting code across: **`bcmd.c` does not port.** The
forwarding-table programming does — the `bcm_l3_*` calls are the same API —
and the packet path does not, because on this side there is no kernel module to
install filters into.

Port naming follows from the same split. EdgeNOS's interfaces are `geN`/`xeN`
because KNET names them after the chip's ports. NOSaic's are `swpN` because the
tap devices are ours to name, and because config that says `swp1` is the only
thing that makes the same command mean the same thing on a different switch.

## The boot chain

```
  power on
     │
     ▼
  U-Boot 2012.10  (NOR mtd0, 896 KiB)
     │  ethaddr=04:F8:F8:15:A8:40   baudrate=115200   consoledev=ttyS0
     │
     ├── bootcmd: run boot_diag; run check_boot_reason; run nos_bootcmd; run onie_bootcmd
     │                                                        │              │
     │                          the NOS, if one is installed ─┘              │
     │                          ONIE, otherwise, or on request ──────────────┘
     │                            cp.b 0x1e100000 → RAM, bootm   (NOR mtd3)
     ▼
  nos_bootcmd
     │  usb start; usbiddev            ← the disk is behind USB and is not
     │                                   reachable until this has run
     │  usbboot 0x70000000 ${usbdev}:1 ← NOSaic: raw read of partition 1
     │  bootm 0x70000000#nosaic
     ▼
  FIT at 0x70000000
     ├── kernel   zImage, load/entry 0x61008000
     ├── ramdisk  placed by U-Boot (initrd_high=0xffffffff — used in place)
     └── fdt      placed by U-Boot
     ▼
  Linux, then NOSaic's initramfs: find the slot pointer, mount the squashfs,
  overlay it, switch root
```

The vendor's own command is the same shape with `ext2load usb 0:1 <addr>
<file>` instead of `usbboot`, because ONL keeps its FIT as a file on an ext2
partition. **This U-Boot can do either** — both commands are in its binary —
and NOSaic uses the raw read only because the image builder cannot yet place a
FIT as a file on the boot partition it already creates. See
[todo.md](todo.md).

### What the firmware can and cannot do

Read out of `mtd0` by dumping it and looking, rather than assumed:

| | |
|---|---|
| Version | `U-Boot 2012.10-gd563f4a (Apr 13 2017)` |
| Partition tables | **GPT and DOS** — `is_gpt_valid`, `print_part_efi`, `get_partition_info_efi` are all present |
| Filesystems | ext2 (`ext2load`, `ext2ls`), FAT (`fatload`). No ext4. |
| Block sources | USB (`usb start`, `usbiddev`, `usbboot`), TFTP |
| Boot formats | FIT (`bootm`), raw zImage (`bootz`) |
| FIT digests | **crc32 and sha1 only** — no md5, no sha256 |

The GPT line is worth dwelling on because the AS5610 is the opposite case: that
board's U-Boot contains no EFI or GUID partition strings at all, so a GPT
install there produces a switch that finds nothing on its own disk. This one
could use GPT. It currently does not, and the reason is on our side rather than
the firmware's — see `partition_table` in `board.yml`.

The digest line has a nasty failure mode. A FIT hashed with something this
firmware cannot compute transfers fine, reports the configuration it found, and
*then* fails with a message about not being able to get the kernel image —
which reads like a corrupt image rather than an unsupported hash.

## Memory and register map

| Range | What |
|---|---|
| `0x18000000` + 1.1 MB | iProc AXI peripherals |
| `0x18000400` | console UART, ns16550a, **62.5 MHz reference** |
| `0x18022000` | AMAC (management port), IDM at `0x18110000` |
| `0x1802a000` | EHCI — the disk |
| `0x18027200` | QSPI — NOR flash |
| `0x18038000` | i2c-0, the board controller bus |
| `0x1803b000` | i2c-1, the mux bus |
| `0x19000000` + 140 KB | Cortex-A9 mpcore: GIC, TWD, PL310 |
| `0x48000000` + 256 KB | **the switch chip's CMIC** |
| `0x61000000` + 768 MB | DRAM |

DRAM does not start at zero, which is why the kernel loads at `0x61008000` and
why an ARM board's usual `0x00008000` would jump into nothing.

## The port map

Three numbering schemes exist and they are not the same. Confusing them has
already cost this project a wrong uplink address.

| Scheme | Looks like | Where it comes from |
|---|---|---|
| NOSaic port | `swp1`..`swp54` | what config and the CLI use |
| ASIC logical | 1..49, 50..55 | the Helix4 itself |
| Front panel | 1..52 plus STK1/STK2 | printed on the metal |
| EdgeNOS netdev | `ge0`..`ge47`, `xe0`..`xe5` | the other project, for cross-reference |

**The first three are related by arithmetic, and Helix4 is the reason.** On the
Trident boards, which logical port reaches which SerDes lane is board wiring
that has to be read off the vendor's OS — it is why those boards ship a
`mkportmap.sh` and gitignore its output. Helix4's mapping is fixed in the
silicon, so there is no `portmap_<n>` property in this board's configuration at
all:

- **copper**: ASIC logical port *N* for `ge`*(N-1)*, N = 1..48
- **SFP+**: ASIC logical port 50+*N* for `xe`*N*, N = 0..5
- `xe0`..`xe3` are the four SFP+ uplinks; `xe4` and `xe5` are the stacking
  ports, and have no PHY entry at all, which is what distinguishes them

The GE bitmap is `0x3fffffffffffe` — bits 1 to 49, **49 bits for 48 ports**.
One logical port in that range is enabled and is not brought out to a cage.
Which, and why, is not established. It is copied rather than trimmed, because a
bitmap that disagrees with the chip's port configuration fails bring-up without
a useful message.

### The external PHYs, and the one that breaks the pattern

Six BCM54282 octal parts on MDIO, at primaries `0x01`, `0x09`, `0x11`, `0x19`,
`0x21`, `0x29`, each covering eight ports at offsets 0–7 — so addresses run
`0x01`..`0x30` straight through. The four SFP+ PHYs are BCM84758s at `0x40`..`0x43`.

**`ge11` is at `0x0e` where the arithmetic says `0x0c`.** One exception in
forty-eight, unexplained, and the reason `tools/mkphymap.sh` exists rather than
a formula in a config file. A wrong PHY address does not error: the SDK talks
to a real PHY belonging to a different socket, and a port reports another
port's link state.

### The front panel is the one that is not known

**Nothing in software maps a silkscreen number to a port.** Three points have
been established by observation on the lab unit:

| Front panel | EdgeNOS netdev | ASIC port | How |
|---|---|---|---|
| 1 | `ge25` | 26 | the console server, proven by ping and a matching neighbour entry |
| 3 | `ge24` | 25 | the PDU's DHCP DISCOVER arrived there |
| 52 | `xe3` | 53 | the Nexus fibre — only `xe3` receives |

The copper block runs **backwards** relative to the silkscreen (1 → `ge25`,
3 → `ge24`) and the fibre block runs forwards. That rules out both obvious
rules — it is not `ge(N-1)`, and it is not a constant offset, since the offset
is +24 at panel 1 and +21 at panel 3. Two points in the copper block cannot
distinguish "top row descending" from "netdevs transposed in pairs", and
guessing between them is how the wrong uplink address happened.

One more copper data point settles it: plug something into a port whose number
you can read and see which port counts.

## I²C topology

```
  i2c-0  (0x18038000, 400 kHz)
     └── 0x30   board CPLD ── fan duty, fan tach, PSU status, PHY resets

  i2c-1  (0x1803b000, 100 kHz)
     └── 0x70   PCA9548 8-channel mux, idle-disconnect
           ├── ch0 → i2c-2   0x50  SFP+ xe0   (panel 49)
           ├── ch1 → i2c-3   0x50  SFP+ xe1   (panel 50)
           ├── ch2 → i2c-4   0x50  SFP+ xe2   (panel 51)
           ├── ch3 → i2c-5   0x50  SFP+ xe3   (panel 52)
           ├── ch4 → i2c-6   0x50  SFP+ xe4   (STK1)
           ├── ch5 → i2c-7   0x50  SFP+ xe5   (STK2)
           ├── ch6 → i2c-8   0x50/0x51  PSU EEPROMs
           │                 0x58/0x59  PSU PMBus (3Y-Power YM-1921)
           └── ch7 → i2c-9   0x48  LM77 temperature
                             0x50  board EEPROM (24c04)
                             0x68  DS1307 RTC — DISABLED, see below
```

`i2c-mux-idle-disconnect` is not optional. Six SFP cages all answer at `0x50`,
so a mux that leaves the last channel selected returns whichever transceiver
was addressed most recently.

## Platform hardware

The driver is `internal/platformhal/as4610/`, registered as `as4610-cpld`, and
it is **Go rather than C** — the first board HAL in this tree that is. The
AS5610's is C only because the gc toolchain has never targeted 32-bit
big-endian PowerPC; `GOARCH=arm` it targets fine, so this board runs the same
CLI as every x86 one and its HAL lives where the contract does.

It is also the first HAL to reach its hardware over plain Linux i2c rather than
through an SCD's SMBus accelerators, which is why `platform_hal` in `board.yml`
states an `i2c:` map instead of an `smbus:` one.

| What | Where | Notes |
|---|---|---|
| Fan duty | CPLD `0x2b`, low nibble | eight steps; duty % ≈ `(n × 125 + 5) / 10` |
| Fan 1 tach | CPLD `0x2d` | rpm ≈ `raw × 379 × 60 / 2 / 100` |
| Fan 2 tach | CPLD `0x2c` | same |
| PSU status | CPLD `0x11` | PSU *i* at bits `2i` (present) and `2i+1` (ok) |
| PHY resets | CPLD `0x07 0x08 0x0d 0x19 0x1b` | all five; released by `nosaic platform release-asic` |
| Temperature | LM77 at i2c-9 `0x48` | one sensor; `hwmon0` on the running box |
| Identity | 24c04 at i2c-9 `0x50` | ONIE TlvInfo |
| Transceivers | `0x50` on i2c-2..7 | six cages behind the mux |

The register map is EdgeNOS's, which took it from Open Network Linux's
`accton_as4610_{fan,psu}.c`. Three things about it are worth knowing before
trusting a reading.

**One reading does not fit.** With the duty register at `0x08` — full speed by
that formula — both tachometers read `0x00` on the lab unit, and the fans are
certainly turning. Either the decode is wrong, or those registers need
something done first, or they are not the registers. The driver reports RPM as
read and carries the raw register beside it rather than smoothing it into
something plausible, and the cooling loop takes its input from the temperature
rather than from this — a loop built on a tachometer that reads zero is a loop
that will decide the fans have failed.

**The PSU decode is the vendor's and is unchecked.** The AS5610's turned out to
be two registers rather than two fields of one, with presence active low, and
its own vendor documentation had it wrong. Pull a power lead and watch which
bit moves before believing this one.

**The fan floor is 50% on purpose.** Declaring a HAL driver is what starts the
cooling loop, so the first boot of an image built from this takes the fans off
full — on a chassis whose only known reading is 49 °C at full speed. Half duty
costs noise and no risk until somebody has a curve.

**Transceiver pages cannot be selected.** Six cages behind one mux all answer
at `0x50`, so a page left selected on one module is selected on whichever the
mux next connects, and a read of another cage then returns its upper page
decoded as its lower one — plausible numbers rather than an error. The driver
refuses a page select by name. SFPs report everything; QSFPs lose their vendor
strings and keep their light levels.

## Quirks

**The real-time clock is deliberately disabled, and enabling it can stop the
board booting.** The vendor ships two device trees for this switch and the one
it boots is named `rtcdis` for this reason. A DS1307 with a flat backup cell
wedges the boot on the i2c mux — no message, and the fault presents as the mux
rather than the clock behind it. Replace the cell before touching that node.

**The console UART's reference clock is 62.5 MHz, and mainline says 25.** The
`bcm-hr2.dtsi` in the kernel tree feeds this UART the oscillator directly.
Take that node unaltered and the switch boots perfectly and every character of
its console is garbage — on a board whose console is the only way to watch a
first boot.

**Mainline's device tree for this SoC family is close and not complete.**
`ARCH_BCM_HR2` exists and `bcm-hr2.dtsi` covers most of the peripherals, but a
Hurricane 2 is single-core and this part is not, and the dtsi has no USB node
at all — and the disk is behind USB. A kernel built from it boots and then
cannot find a root filesystem.

**The vendor's device tree names drivers that mainline does not have.**
`brcm,xgs-iproc-amac`, `brcm,xgs-iproc-armpll`, `brcm,usb-phy-hx4`,
`brcm,helix4` and others are Broadcom BSP bindings. Copying that tree gives a
console and nothing else. `dts/as4610-54t.dts` uses mainline's names throughout
and disables what mainline cannot drive; the comments in it say which is which.

**Nothing renames network interfaces under NOSaic.** The management port is
`ma1` on this box under both ONL and EdgeNOS, which run systemd; NOSaic runs s6
and the kernel's own name stands. Transcribing `ma1` out of `ip addr` on the
running switch produces a management interface that is silently never
configured — exactly the way the AS5610's `eth0`/`end0` mix-up failed.

## Provenance

Split three ways, because they are worth different amounts.

**Measured on the lab unit**, over a read-only ssh session on 2026-09-12: the
CPU feature list and part number, DRAM size, the platform device list, the
partition and flash layout, the i2c topology, the LM77 and CPLD register reads,
the U-Boot environment, and the U-Boot binary's capabilities (dumped from
`mtd0` and read). The FIT addresses and digest come from the vendor's own
`arm-accton-as4610-54-r0.itb`, and the device tree in this directory is derived
from the DTB extracted out of it.

**Read out of upstream source**: which mainline drivers exist and what they
match, from Linux 6.12.105; the SDK's Helix4 support and its iProc platform
definition, from OpenBCM 6.5.24.

**Design, not observation**: the UIO section and the packet-flow diagram's
left-hand side. Neither has been run. They are the plan `nosd-helix4` is meant
to implement, written down first so that the thing that fails is a stated
expectation rather than an assumption.

The investigation behind the EdgeNOS-derived numbers — the CPLD registers, the
fan and PSU map — lives in that project rather than here. Vendor SDK source is
never copied into this tree; it is referenced by `file:line`.
