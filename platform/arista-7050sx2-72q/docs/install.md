# Installing NOSaic on the 7050SX2-72Q

> **Not yet possible.** No NOSaic image has been installed on this switch. This
> page records the mechanism, which is established from the switch's own flash,
> so that the first attempt is not also the first time anyone writes it down.

## Before you start

Installing over EOS is reversible **only if you have the EOS SWI**. Copy the
existing `/mnt/flash/EOS-*.swi` and `/mnt/flash/boot-config` off the box before
touching anything. Restoring is a matter of putting them back.

## Console

USB or RJ45 console to **ttyS0, 9600 8N1**, no flow control. Aboot prints on the
same port, so a wrong baud looks like a dead box.

## How Aboot loads an image

Aboot reads `/mnt/flash/boot-config`, a single line:

```
SWI=flash:/NOSaic-<version>-arista-7050sx2-72q.swi
```

It then opens that SWI — a zip — and runs the `boot0` inside it, which kexecs
our kernel. **No signing is enforced on this board**, and Aboot can also load
over HTTP, FTP, TFTP or NFS, which is the safer route during bring-up because
nothing in flash changes.

The board's `prefdl` reports `HwEpoch 1`, so a SWI declaring
`SWI_MAX_HWEPOCH=1` is accepted. Ours does.

## Installing

1. Copy the `.swi` to `/mnt/flash/`.
2. Point `boot-config` at it, keeping the old line commented so it can be put back.
3. `reload`.

## Going back to EOS

Restore the original `boot-config`, or at the Aboot prompt boot the EOS SWI
directly. Aboot itself is not modified by any of this, which is what makes the
switch recoverable.

## When it does not work

Unwritten, deliberately. The failures that belong here are the ones seen on this
board, and there have not been any yet.
