# boot/ — bootloader and installer backends

One module per bootloader, behind a single interface. Each knows one thing: how to
wrap an image in that bootloader's installer envelope.

**Not how to choose an A/B slot**, which is what this said and what the design
originally assumed — a slot-control implementation per bootloader, GRUB counters
against U-Boot `bootcount` against Aboot's `boot-config`. In practice the
initramfs reads the boot pointer and chooses, so the bootloader never learns that
slots exist and the same trial, counter and rollback code runs on every board.
`aboot.go` contains no slot logic at all.

What does vary is where the slots live, and that is confined to two lookups in the
initramfs: partitions on most boards, files on the bootloader's own filesystem on
a board whose bootloader owns the whole disk.

Adding a bootloader is a module plus a data entry — no other file changes.

Merged: `virt`, `aboot`, `onie-sfx`, `uboot`. Aboot boots the 7050SX2 from its own
flash and the ONIE installer put NOSaic on the AS5610's disk, so two of the four
are confirmed on hardware rather than by extracting the artifact. Planned:
`onl-swi` (M8).
