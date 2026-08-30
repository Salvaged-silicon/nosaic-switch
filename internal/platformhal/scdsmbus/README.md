# scdsmbus — GPL-2.0, and the only such package in NOSaic

The SMBus accelerator protocol here is **ported from Arista's GPL-2.0
`scd-smbus.c`** (`scd_smbus_master_xfer`). Transcribing a driver's bitfield
layout and transfer sequence produces a derivative work, so the licence follows
it. NOSaic is otherwise Apache-2.0.

The line matters and is worth stating precisely, because most of what NOSaic
takes from Arista's open sources falls on the other side of it:

| | |
|---|---|
| Reading a register **map** — an address, a bit position | fact-gathering, no licence attaches |
| Transcribing a **protocol** — request word layout, transfer sequence, reset handling | derivative work, GPL-2.0 follows |

So the SCD reset block, the watchdog, PSU presence and the transceiver cage
table are all read from addresses Arista publishes and live in the Apache-2.0
`scd` package. This one does not.

## Why it is a separate package

So the boundary is a package boundary rather than a comment somebody has to
notice. A file header is easy to miss when code moves; an import is not.

It depends on nothing else in NOSaic — register access is injected through the
`Registers` interface — so it can be lifted out whole, replaced with a
clean-room implementation, or dropped by a board that has no SMBus.

EdgeNOS reached the same conclusion about its own `scdreset.c`, which is
GPL-2.0 for exactly this reason.
