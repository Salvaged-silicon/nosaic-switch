# Dell S6000-ON Hardware

## CPU, Memory, and PCI

Observed on a running Dell S6000-ON (`x86_64-dell_s6000_s1220-r0`):

- **CPU**: Intel Atom S1220 (Centerton), 8 GiB RAM
- **e820 usable RAM**:
  - `0x0000000000100000 - 0x00000000bed0cfff` (~3053 MiB below 4 GiB)
  - `0x0000000100000000 - 0x000000023fffffff` (above 4 GiB)
  - Because `0xd0000000` sits in the PCI hole, `kernel_params` reserves
    `memmap=64M$0xb0000000 iomem=relaxed` for `nosd-td2`'s userspace BDE.
- **PCI devices**:
  - `01:00.0` — Broadcom BCM56850 Trident II (`14e4:b850`, rev `03`)
  - `02:00.0` — Marvell 88SE9170 SATA controller (`1b4b:9170`, 14.9 GiB SSD)
  - `00:13.0` / `00:13.1` — Intel Atom S1200 SMBus 2.0 (`8086:0c59`, `8086:0c5a`, `i2c_ismt`)
  - `00:1f.0` — Intel Atom S1200 Integrated Legacy Bus (`8086:0c60`, `i2c_isch`)
  - `08:00.0` — Intel 82574L Gigabit Ethernet (`8086:10d3`, `e1000e`)

## Disk & Boot

- **Boot mode**: Legacy BIOS + GPT
- **Partitions**: `sda1` `GRUB-BOOT` (2 MiB), `sda2` `ONIE-BOOT` (128 MiB ext4),
  followed by NOSaic's `nosaic-boot`, `nosaic-slot-a`, `nosaic-slot-b`, and
  `nosaic-data`.

## Platform Hardware (I2C / CPLDs / Sensors)

Observed on the running switch (`i2c-0` = iSMT at `ff782000`, `i2c-1` = iSMT at
`ff781000`, `i2c-2` = SCH at `0400` behind a 4-channel `i2c_mux_gpio` -> `i2c-10..13`):

- **PSUs (`i2c-1`)**: `1-0058` / `1-0059` (`dni_dps460` PMBus), `1-0050` / `1-0051` (`24c02` EEPROM)
- **CPLDs & CPU (`i2c-10`, mux ch 0)**:
  - `10-0031`, `10-0032`, `10-0033` (System-CPLD, Master-CPLD, Slave-CPLD)
  - `10-004d` (`emc1403`: CPU0, CPU1, CPU internal DTS)
  - `10-0018` (`jc42`: DIMM), `10-0053` (`24c02`: Board / ONIE TlvInfo EEPROM)
- **Fans & Board Sensors (`i2c-11`, mux ch 1)**:
  - `11-0029`, `11-002a` (`max6620`: 3 fan trays × 2 fans = 6 fans, minimum 40% duty)
  - `11-004c`, `11-004d`, `11-004e` (`tmp75` / `lm75`: ASIC On-Board, NIC, System Front)
  - `11-0040`, `11-0042` (`ltc4215`: hot-swap controllers)
- **QSFP+ Cages (`i2c-12`, `i2c-13`, mux ch 2–3)**:
  - `i2c-20..35` (`0x50`) and `i2c-36..51` (`0x50`) for all 32× QSFP+ EEPROMs

## SerDes, Lane Swaps, Polarity, and Breakout

- All 32 front-panel ports are direct-attach 40GbE QSFP+ (no external copper
  PHYs).
- Each quad has PCB-specific RX/TX lane swaps (`xgxs_rx_lane_map_*`,
  `xgxs_tx_lane_map_*`), polarity flips (`phy_xaui_rx_polarity_flip_*`,
  `phy_xaui_tx_polarity_flip_*`), and SerDes TX equalization
  (`serdes_driver_current_*`, `serdes_pre_driver_current_*`,
  `serdes_preemphasis_*`). All of these are extracted into `config/portmap.conf`
  by `tools/mkportmap.sh`.
- **4×10G Breakout**: In 40G mode the 32 cages occupy logical ports `1..32`.
  Breaking out a cage into 4×10G (either statically via `QSFP_BREAKOUT=1` or
  live via `nosaic port breakout <port> 4x10g`) allocates three free logical
  ports in `33..128` and persists `nosaic_portmode_<port>=4x10g` to
  `/mnt/data/config/portmode.conf`.

## Useful Bring-Up & Recovery Notes

- **ONIE Recovery**: `ONIE-BOOT` (`sda2`) and `GRUB-BOOT` (`sda1`) are preserved
  by the installer, and the original ONIE `grub.cfg` is backed up to
  `grub.cfg.onie` and chained as the second GRUB menu entry.
- **No Device Tree on x86_64**: Unlike ARM/PowerPC, `i2c_ismt` (`i2c-0`,
  `i2c-1`) and `i2c_isch` (`i2c-2`) do not auto-instantiate devices behind the
  GPIO mux (`i2c-10..51`) without ACPI or explicit `new_device` / driver
  binding. `internal/platformhal/s6000/` reads `/sys/class/hwmon` once bound;
  binding the mux/CPLDs is tracked in `docs/todo.md`.
- **CPLD & QSFP+ Control**: Under SONiC, the out-of-tree `dell_s6000_platform`
  module exposes `/sys/devices/platform/dell-s6000-cpld.0/` (`psu{0,1}_prs`,
  `psu{0,1}_status`, `qsfp_modprs`, `qsfp_modsel`, `qsfp_lpmode`, `qsfp_reset`)
  backed by the three I2C CPLDs (`10-0031`, `10-0032`, `10-0033`) and
  `i2c_mux_gpio` (`i2c-20..51` at `0x50`). Direct I2C support for those CPLD
  registers in `internal/platformhal/s6000/` is tracked in `docs/todo.md`.
