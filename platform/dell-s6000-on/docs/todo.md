# Dell S6000-ON Todo

- [x] Extract `portmap.conf` from `td2-s6000-32x40G.config.bcm` (32× 40G QSFP+).
- [x] Boot on hardware (`--ram-boot`), verify `nosd-td2` attaches `14e4:b850`,
      links 40G QSFP+ ports, and switches `nosaic port breakout <port> <40g|4x10g>`.
- [x] Implement SCH GPIO-muxed userspace SMBus MAX6620 fan control and
      TMP75/EMC1403 temperature sensors in `internal/platformhal/s6000/`.
- [ ] Install permanently to SSD (`NOSaic-<version>-dell-s6000-on.bin`) and
      verify `GRUB-BOOT` + `ONIE-BOOT` preservation.
- [ ] Wire CPLDs (`10-0031..33`), QSFP+ EEPROM/control, and ONIE TlvInfo board
      EEPROM (`10-0053`) in `internal/platformhal/s6000/`.
