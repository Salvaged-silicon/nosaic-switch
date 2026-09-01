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

## Flash

NOR flash, as MTD partitions rather than a block device:

```
mtd0   0x00360000   onie
mtd1   0x00010000   u-boot-env
mtd2   0x00010000   board_eeprom
mtd3   0x00080000   uboot
```

Two things follow. There is no eMMC and no partition table, so the
flash-resolution work done for Aboot on the 7050SX2 does not transfer — an
image goes on through ONIE, and NOSaic's own persistent state has to live
somewhere an MTD can hold it. And `board_eeprom` is its own partition, which is
where the board's identity and MAC live; the 7050SX2 keeps the same information
on an i2c SEEPROM.

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

## Sensors

`hwmon0` and `hwmon1` are present under the vendor OS. Neither has been
identified, and the platform HAL for this board does not exist.

## What has not been established

Everything the datapath needs:

- the port map — which SDK logical port is which front-panel cage
- SerDes polarity, if this board swaps pairs as the 7050SX2 does
- how the chip is released from reset, and by what
- whether the DMA region is reachable the same way

The vendor OS on the lab unit has all of it, in `/etc/edged/`, in the same
shape the 7050SX2's did. That is where the generators should be pointed when
somebody starts this port.
