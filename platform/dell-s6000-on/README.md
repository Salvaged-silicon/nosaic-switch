# Dell Networking S6000-ON

32× 40GbE QSFP+ data-center switch built on a Broadcom BCM56850 (Trident II)
and an Intel Atom S1220 (x86_64) CPU.

| Field | Value |
|---|---|
| Status | `bringup` |
| ONIE platform | `x86_64-dell_s6000_s1220-r0` |
| Architecture | `x86_64` (Intel Atom S1220) |
| ASIC | `td2` (BCM56850 `14e4:b850` rev `03`) |
| Bootloader | `onie-sfx` (Legacy BIOS + GPT + GRUB2) |
| Console | `ttyS0` at `115200` 8N1 |

## Pages

- [install](docs/install.md) — installing NOSaic from ONIE
- [build](docs/build.md) — building an image and extracting `portmap.conf`
- [hardware](docs/hardware.md) — PCI, e820 memory map, I2C, sensors, and CPLDs
- [todo](docs/todo.md) — remaining bring-up items
