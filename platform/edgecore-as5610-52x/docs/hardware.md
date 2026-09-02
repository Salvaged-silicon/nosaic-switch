# Edgecore AS5610-52X — hardware reference

Everything here was read off a running unit. Nothing is inferred from a
datasheet, and nothing has been produced by NOSaic, which has never run on
this board.

## At a glance

```
model            powerpc-accton-as5610-52x-r0   (device tree)
CPU              Freescale P2020, e500v2, 32-bit big-endian, 2 cores
memory           2 GB
ASIC             BCM56846 (Trident+), 14e4:b846 rev 02, at 0001:01:00.0
front panel      48 x SFP+ 10G  +  4 x QSFP+ 40G
console          ttyS0, 115200
kernel cmdline   console=ttyS0,115200 cma=32M
```

Note the PCI domain: `0001:01:00.0`, not `0000:`. A P2020 has more than one
PCIe controller and the ASIC is not on domain zero — anything that hardcodes
`0000:` will not find this chip.

`cma=32M` on the command line is worth carrying forward. The Trident+ needs a
contiguous DMA region the same way the Trident2+ does; on the Arista that is a
`memmap=` reservation, and here it is CMA.

## Ports

The SDK's own port groups, from the vendor OS:

```
allports   xe0-xe51
10Gports   xe0-xe47      SFP+
40Gports   xe48-xe51     QSFP+
```

52 logical ports, contiguous, with the four 40G cages at the top. That is a
simpler arrangement than the 7050SX2's, where the QSFP cages sit at logical
49/53/57/61/65/69 with gaps for breakout lanes.

## Storage

Two devices, and the distinction decides where an image goes.

**NOR flash, 8 MB, as MTD** — firmware only:

```
mtd0   0x00360000   onie
mtd1   0x00010000   u-boot-env
mtd2   0x00010000   board_eeprom
mtd3   0x00080000   uboot
```

**NAND, ~3.8 GB, as an ordinary block device** — `/dev/sda`, with a partition
table: `sda1` 127 MB, `sda3` 3386 MB, `sda5` 15.9 MB, `sda6` 289 MB.

That second device is the good news for this port. The vendor OS runs the exact
shape NOSaic was designed around — a read-only squashfs under an overlay:

```
overlay on /  lowerdir=/lower  upperdir=/rw/config1/upper  workdir=/rw/config1/work
```

`config1` rather than `config` suggests slots, and the switch database entry
records `persistence: squashfs-overlay` with dual-slot A/B upgrade. So the A/B
machinery that is CI-tested on the virtual platform and has never run on the
Arista has a plausible home here.

`board_eeprom` is its own MTD partition and holds the board's identity and MAC
addresses; the 7050SX2 keeps the same information on an i2c SEEPROM read
through its board controller.

## S-Channel, and the big-endian trap

**This is the single most valuable thing on this page**, and it is somebody
else's work: EdgeNOS ported the full OpenBCM SDK to this chip and made
S-Channel complete on real silicon. The findings are recorded here so this
port does not rediscover them, and are attributed rather than claimed —
nothing below has been reproduced by NOSaic.

The BCM56846 is a **CMICe** chip — the S-Channel engine is at
`CMIC_SCHAN_CTRL = 0x50`, and it is *not* CMICm, which is what the SDK
assumes for parts of the 56840 family. Five separate fixes were needed
(EdgeNOS `patches/sdk-6.5.16-bcm56846-fixes.patch`):

1. `feature.c`, `soc_features_bcm56840_b0` — do **not** force cmicm or
   `new_sbus_format` for `0xb846`; do enable `soc_feature_schmsg_alias`,
   because the chip uses the `0x800` message-buffer alias.
2. `schan.c`, `soc_schan_init` — remove the `|| 0xb846` cmicm force so it
   takes `soc_cmice_schan_init`.
3. `drv.c`, `soc_endian_config` — force
   `CMIC_ENDIAN_SELECT = 0x04000004` (`ES_BIG_ENDIAN_DMA_OTHER`, **PIO endian
   OFF**).
4. `schan.c`, `soc_schan_header_cmd_set` — force `src_blk = 0` for `0xb846`.
   The chip has `SCHAN_SB0`, the reply routes to `src_blk`, and `CMIC_BLOCK=5`
   sent the ACK to the wrong block.
5. the `schmsg_alias` feature again — the `0x800` buffer alias.

Item 3 is the one to understand before touching this board, because it is the
first genuinely **big-endian** problem NOSaic will meet. The SDK's default
endian select adds `ES_BIG_ENDIAN_PIO`, which makes the CMIC byte-swap the
CPU-side PIO access to the S-Channel message buffer. The engine then reads a
garbage message, `MSG_START` never latches, and the operation times out. The
symptom is an S-Channel that does nothing and reports nothing, on a host where
every register access otherwise appears to work.

The 7050SX2 never met any of this: it is little-endian x86 talking to a CMICm
chip. Every assumption NOSaic's Trident2+ datapath makes about S-Channel
should be treated as unverified here.

With those fixes, on this silicon: `soc_init` completes (411,530 S-Channel
operations, tables cleared over S-Channel rather than DMA), `bcm_init`
completes, and `bcm_field` creates and installs a TCAM entry that reads back
correctly.

## Board hardware around the ASIC

From EdgeNOS's board manifest, none of it verified by NOSaic:

| | |
|---|---|
| CPLD | `accton_as5610_52x_cpld` — its own driver |
| 40G retimer | **DS100DF410**, on the QSFP path, with an init service |
| Sensors and SFP | ONLP platform layer |
| LEDs | **led0.hex / led1.hex** — microcontroller firmware images |
| Device tree | `as5610-52x.dts` |

The retimer matters: the 40G path is not direct SerDes to cage as it is on the
7050SX2, and a retimer that has not been programmed is a link that does not
come up. The sibling 7050TX-64 has the same class of part (a DS100KR800) and
its bring-up notes record that leaving it in reset costs every port on the box,
not just the 40G ones.

The LEDs are the opposite of the 7050SX2's arrangement. There the chip's LED
microprocessors are disabled and the board controller drives the panel through
memory-mapped registers; here there is microcontroller firmware to load, which
is the mechanism NOSaic has not implemented on any board.

## Sensors, fans and PSUs

Read off the running unit on 2026-09-02. The platform HAL for this board does
not exist yet; this is what it has to talk to.

### The CPLD is where the box hardware lives

Memory-mapped on the localbus at physical **`0xEA000000`**, 0x100 bytes — not
on I2C. Fans, PSUs, the front LEDs, reset and the watchdog are all behind it.

| Offset | Contents |
|---|---|
| `0x00` | version (reads `0x10` on this unit) |
| `0x01` | **PSU2** status |
| `0x02` | **PSU1** status |
| `0x03` | fan status |
| `0x0d` | fan PWM |
| `0x10` | reset |
| `0x11` | interrupt status |
| `0x13` | `led_sys` |
| `0x15` | `led_loc` |

A live dump, for shape:

```
00: 10 00 02 10 00 07 ea ea 0f 07 aa fa fa 14 00 fa
10: ff ff 00 6f ff 01 ea ea ea ea ea ea 08 ea ea ea
```

### PSUs: active-low, and PSU1 is in the higher register

**present = bit0 is `0`**, power-good = bit1. PSU1's status is in `0x02` and
PSU2's in `0x01` — the order is inverted relative to their names, which is not
a typo here.

This matters because EdgeNOS's own CPLD driver gets it wrong, and says so: its
`psu1_present` reads bit0 of `0x01` active-*high*, so on a running switch both
PSUs report absent. EdgeNOS's Python HAL bypasses the driver and decodes the
registers itself, with the comment that the map below is the Cumulus-proven
one. Decoding this unit's dump that way:

```
PSU1  reg 0x02 = 0x02  -> present, power-good
PSU2  reg 0x01 = 0x00  -> present, NOT power-good
```

which is a plausible reading of a switch with a second supply fitted and not
energised. It is not proof: an empty bay might also read `0x00`. Distinguishing
them means looking at the box, and until someone does, treat PSU2's state here
as unconfirmed rather than as a fault to chase.

### Fans: four, one PWM between them, and the scale is 5 bits

Four fans, a single board-wide status register and a single PWM. **No per-fan
tachometer** — nothing reports RPM, so "is that fan spinning" is not a question
this hardware answers. `hwmon` shows no fan at all; it is CPLD-only.

**The PWM is 0–31, not 0–255.** From the working fan controller:

| PWM | Duty | Threshold |
|---|---|---|
| 10 | ~32% | below 40 °C |
| 14 | ~45% | 40 °C |
| 20 | ~64% | 50 °C |
| 26 | ~83% | 60 °C |
| 31 | 100% | 72 °C critical, 75 °C halts after ~30 s |

EdgeNOS's Python HAL has this wrong in the other direction: `fan_set()`
computes `pct * 255 / 100` and clamps to 255, writing into a five-bit field. It
happens to be right at 100%; `fan_set(50)` produces 127, whose low five bits
are 31, so asking for half speed gives full speed. The shell fan controller in
the same tree uses the correct 0–31 scale, so the two disagree with each other.

### The I2C mux tree, and how a port becomes a bus number

**70 buses.** Two controllers, and everything else is behind muxes.

```
i2c-0  board control    pca9548 @0x70 -> ch0 RTC(2-0051)  ch1 PSU1 eeprom
                                         ch2 PSU2 eeprom  ch4 USB hub
                                         ch5 VT1165M      ch6 ICS83905I
                                         ch7 max6697 + ne1617a

i2c-1  ports            pca9546 @0x75 -> 4 x pca9548 @0x74  SFP  1-32
                        pca9546 @0x76 -> 2 x pca9548 @0x74  SFP 33-48
                                         ch2 -> bus 64  four pca9538 (GPIO)
                                         ch3 -> bus 65  SFP status  <-- NOT IN THE DTS
                        pca9546 @0x77 -> 4 buses           QSFP 49-52
```

Each port bus carries the module's EEPROM at `0x50` (and its DOM page at
`0x51`) plus a **DS100DF410 retimer at `0x27`** — one per port, on the port's
own bus, which is why the retimer is reached through the mux like everything
else.

**Two different parts, and the difference is not just width.** Three PCA9546
(4-channel) and seven PCA9548 (8-channel). Both are *switches* rather than
one-hot multiplexers: the channel-select register is a bitmask and more than
one channel can be open at once.

That is the whole reason isolation matters here. Every port channel presents
**the same three addresses** — `0x50`, `0x51`, `0x27`. Two channels open
together means two EEPROMs answering as `0x50`, and a read returns whichever
device wins the bus. The failure is not an error; it is optic A's serial number
reported for port B.

`i2c-mux-idle-disconnect` is what closes the channel when the transfer ends.
The load-bearing one is the **root** mux on each branch: nothing behind a
sub-mux is reachable unless its parent has that branch open, so a disconnecting
PCA9546 isolates all eight ports below it whatever the PCA9548 does.

The running EdgeNOS device tree applies it to the three port muxes and not to
the board-control PCA9548 — defensible, since that mux's channels carry
distinct addresses (RTC `0x51`, PSU EEPROMs `0x3a`/`0x39`, thermal `0x4d`/`0x18`)
and have nothing to collide over:

```
mux@70 (board control)  compatible i2c@0 i2c@1 i2c@2 i2c@7 name reg
mux@75 (ports)          compatible i2c-mux-idle-disconnect i2c@0 ... reg
                                   ^ present here, absent above
```

NOSaic's copy of the device tree declares it on **all ten**. That is stricter
than what is running and costs a few extra bus transactions; it is kept because
the cost of being wrong is silent cross-talk between ports, and no amount of
staring at a bus trace makes that obvious after the fact.

Bus numbers are allocated by probe order, so they are a property of this device
tree rather than of the board. Verified against the running unit:

```
SFP  port p (1-48):  bus = 11 + 9 * ((p-1) / 8) + ((p-1) % 8)
QSFP port p (49-52): bus = 66 + (p - 49)
```

Nine buses per group of eight ports: the sub-mux takes one (buses 10, 19, 28,
37, 46, 55) and its eight channels take the rest.

**Do not hard-code these, and the evidence is in EdgeNOS's own tree.** The bus
numbers moved between kernel versions, and three different answers survive
side by side:

| Source | SFP ports | QSFP | GPIO |
|---|---|---|---|
| `onlp/sfpi.c` | buses 22-69 | 18-21 | 17 |
| `onlp/platform_lib.h` | `21 + port` | `port - 49 + 18` | — |
| `platform.py` | `11 + 9*((p-1)/8) + ((p-1)%8)` | `66 + (p-49)` | 64 |

Only the last matches the running switch. The addresses never moved; the bus
numbers did, because they are allocated in probe order and the probe order is a
property of the kernel and the device tree, not of the board.

So the formula above documents *this* device tree and should not be written
into code. The durable way to find a port's bus is to walk it: the mux devices
appear in sysfs as `i2c-N/of_node` pointing at `.../i2c@3100/mux@75/i2c@0/...`,
so a port's bus can be resolved from the topology that actually loaded rather
than from a number someone wrote down against a kernel that has since been
replaced.

### GPIO: four expanders on bus 64, and four more the device tree forgot

Bound and working, `pca9538` on ch2 of the `0x76` mux:

| Device | gpiochip | Contents |
|---|---|---|
| `64-0070` | 504 | QSFP `RESET_L[3:0]` on pins 0-3, `MODSEL_L[3:0]` on pins 4-7 |
| `64-0071` | 496 | QSFP `MODSEL`/`INT` |
| `64-0072` | 488 | SFP 40-47 rate select |
| `64-0073` | 480 | misc |

Pins 0-3 driven high deassert QSFP reset. The per-module control byte is bit0
`MODSEL_L` (0 = selected), bit1 `RST_L` (1 = not reset), bit2 `LPMODE`
(0 = high power) — so `0x02` is a selected, running, full-power module.

**Bus 65 — ch3 of the same mux — has no Linux devices on it**, because the
device tree describes its contents in comments and declares no nodes. The chips
are there; a read-only probe of the running switch:

```
bus 65:  0x20 ACK -> 0x00   0x21 ACK -> 0x00
         0x22 ACK -> 0x00   0x23 ACK -> 0xff
```

Those four expanders carry **SFP MOD_ABS, TX_FAULT, RX_LOS and TX_DISABLE for
ports 1-48**. ONLP has the map — `0x20` = MOD_ABS ports 0-39, active low =
present; `0x23` = ports 40-47 plus QSFP presence — and calls them PCA9506, a
40-pin part, which fits "ports 0-39" better than the 8-pin `pca9538` the device
tree names on the neighbouring channel.

**This is not a blocker, and it is worth being precise about why.** EdgeNOS
drives all of it and has no device tree nodes either: it opens `/dev/i2c-N` and
does raw transactions.

```c
i2c_read (I2C_BUS_GPIO_CTRL_2, I2C_ADDR_SFP_MOD_ABS,    offset, &val)
i2c_write(I2C_BUS_GPIO_CTRL_2, I2C_ADDR_SFP_TX_DISABLE, offset, &val)
i2c_read (I2C_BUS_GPIO_CTRL_1, 0x70, 0x01, &val)   /* QSFP lpmode */
```

Note the last line: even the four expanders that *are* bound as gpiochips on
bus 64 are driven by raw I2C rather than through `/sys/class/gpio`. So the
board has two ways in, and the vendor stack uses the lower one for everything.

The choice for NOSaic is therefore a design decision rather than a missing
capability:

- **Declare them in the device tree** and get `gpiochip`s, a kernel driver
  handling the register banks, and no bus numbers in our code.
- **Raw I2C**, as EdgeNOS does — proven on this hardware, and it works today
  with no device tree change at all.

The first is tidier and the second is what is known to work. Either way the
information is reachable now.

### LEDs are not on this bus

Worth stating because the mux tree looks like it should own them. It does not.

- **Front-panel port LEDs** come from the BCM56846's two LED microprocessors
  (LEDUP0, LEDUP1) running microcode, driven by the datapath daemon. EdgeNOS
  loads a passthrough microcode and writes link state itself, having found the
  fully-autonomous path lit the wrong ports: the chip-side link bits its
  hardware scan reads are seeded stale at init, and making it correct needs a
  board `PORT_ORDER_REMAP` only obtainable from a live Cumulus capture.
- **System and locator LEDs** are CPLD registers `0x13` and `0x15`.

So the LED work on this board is datapath work plus two CPLD registers, and
none of it goes through I2C.

### Temperatures

`max6697` at `0x4d` on I2C bus 0 channel 7, **seven channels**, reading 31–39 °C
on an idle unit. A second sensor, an `ne1617a` at `0x18`, is on the same
channel. Both are in the device tree and our kernel now has drivers for them.

`bde_tmon` is the third hwmon device and is **not** a board sensor: it is the
Broadcom die's own monitor, from an EdgeNOS kernel module. It reads **150 °C**
on an idle switch. The working fan controller explicitly skips it:

```sh
[ "$(cat .../name)" = "bde_tmon" ] && continue
```

and also discards any reading at or above 120 °C. Any thermal loop NOSaic
writes must do the same, or the fans sit at full forever — or the box halts on
a sensor that is not measuring the board.

## What has not been established

Everything the datapath needs:

- the port map — which SDK logical port is which front-panel cage
- SerDes polarity, if this board swaps pairs as the 7050SX2 does
- how the chip is released from reset, and by what
- whether the DMA region is reachable the same way

The vendor OS on the lab unit has all of it, in `/etc/edged/`, in the same
shape the 7050SX2's did. That is where the generators should be pointed when
somebody starts this port.
