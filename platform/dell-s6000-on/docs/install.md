# Installing NOSaic on Dell S6000-ON

## Before you start

The x86_64 `onie-sfx` installer preserves `GRUB-BOOT` (`sda1`) and `ONIE-BOOT`
(`sda2`) on the internal SATA SSD, removes partitions after `ONIE-BOOT` (such as
`PLATFORM-DIAG` and any previous NOS), and creates `nosaic-boot`, `nosaic-slot-a`,
`nosaic-slot-b`, and `nosaic-data`.

Back up any vendor OS image or license data before running `onie-nos-install`.
ONIE itself remains intact and selectable from the GRUB boot menu.

## Console

RJ45 RS-232 serial console on the management panel: **115200 8N1**, no flow
control (`ttyS0`).

## Installing from ONIE

1. Boot the switch into **ONIE Install** mode from the GRUB menu (or via
   `onie-boot-mode -o install` if currently running another NOS).
2. Transfer or serve `NOSaic-<version>-dell-s6000-on.bin` over HTTP/TFTP/SCP and
   run:
   ```sh
   onie-nos-install http://<server>/NOSaic-<version>-dell-s6000-on.bin
   ```
3. The installer partitions the SSD after `ONIE-BOOT`, populates `nosaic-boot`,
   `nosaic-slot-a`, and `nosaic-data`, updates `ONIE-BOOT/grub/grub.cfg` with
   `NOSaic` (default) and `ONIE` (fallback), and reboots.
