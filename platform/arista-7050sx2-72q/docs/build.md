# Building an image for the 7050SX2-72Q

The general build is in [docs/BUILDING.md](../../../docs/BUILDING.md). Only what
is specific to this board is here.

> **Incomplete.** The base image below builds today; it will boot and do
> nothing, because the datapath does not exist yet. The rest of this page is
> what M6 has to add.

## The short version

```sh
make toolchain ARCH=x86_64
make packages ARCH=x86_64 PROFILE=full
make image BOARD=arista-7050sx2-72q
```

This board is x86_64, the same architecture the virtual platform proves on every
commit, so the toolchain, glibc, kernel and userspace are the ones already
tested — nothing here is a fresh cross-compilation problem.

The `aboot` backend emits `NOSaic-<version>-arista-7050sx2-72q.swi` plus the
`boot-config` line that points Aboot at it.

## What this board needs that the generic build does not

None of this exists yet. Each is a recipe, and each has a licensing question
that must be answered before it can ship in a published image:

| Piece | What it is | Redistributable? |
|---|---|---|
| OpenBCM SDK | Chip support for BCM56860; `sdk-6.5.27` covers it | Licence permits distribution — confirm per file |
| BDE / KNET | Broadcom kernel modules, built against our kernel recipe | Same grant as the SDK |
| `nosd-td2p` | The Trident2+ datapath, implementing `switch-api` | Ours, Apache 2.0 |
| platform HAL | Sensors, PSUs, LEDs, SFP EEPROM | Ours |

The SDK is referenced by `file:line` and never copied into this tree.

## Kernel

The fleet kernel plus a board fragment. Known requirements:

- `CONFIG_I2C_PIIX4` — without it the fan CPLD cannot be reached at all.
- `tg3` for the management port (`14e4:1682`).
- The Arista SCD (`3475:0001`) is a PCI device; its driver is board support we
  supply rather than something mainline provides.

## Verifying before you install

Nothing in this section is a substitute for having the EOS SWI saved. Aboot can
boot an image over TFTP without writing to flash, and during bring-up that is
the right way to try one: a bad image then costs a reboot rather than a
recovery session.
