# boot/ — bootloader and installer backends

One module per bootloader, behind a single interface. Each knows two things: how
to wrap an image in that bootloader's installer envelope, and how to control which
A/B slot boots next.

Adding a bootloader is a module plus a data entry — no other file changes.

Planned: `virt` (M3), `aboot` (M5), `onie-sfx` (M5), `uboot` and `onl-swi` (M8).
