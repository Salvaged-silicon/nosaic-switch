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

## Aboot has no network of its own

It is a separate environment from EOS and comes up with nothing configured, so
an HTTP boot fails with "Network is unreachable" before it fetches anything:

```
initnetdev
ifconfig ma1 <addr> netmask 255.255.255.0 up
route add default gw <gateway> dev ma1
ping -c 3 <build host>          # wait for it: the first attempt fails on ARP
```

Configure it at the prompt rather than setting NETIP and NETGW in boot-config,
which writes flash. A runtime address disappears at the next reboot, which is
what you want for a test.

## Arm the watchdog before booting anything experimental

The SCD watchdog is the recovery path: nothing in NOSaic punches it, so a
wedged image is supposed to reset the board back into EOS. **It is not armed by
default.** It is a register somebody sets, and its state varies -- read it back
with `scdreset status`, where `0x00000000` means disarmed.

Assuming it was live is how the first NOSaic boot on this board ended in a hung
switch and a trip to the PDU. Aboot punching the watchdog during its own boot
does not mean it stays armed across a kexec into something that never touches
the SCD.

## When it does not work

**`The SWI is too old. Please use a SWI with version of at least 4.14.7`** --
Aboot 6.1.2 parses SWI_VERSION as EOS's series.major.minor. NOSaic ships 4.99.0
there and carries its own version as NOSAIC_VERSION.

**`tg3: Could not obtain valid ethernet address, aborting`** -- the MAC is in
the board's prefdl and reaches the NIC through an SCD mailbox, not the NIC's
own EEPROM. boot0 brings the management interface up before the kexec so that
Aboot's driver copies it into the MAC register, and down again before jumping.

**The kernel boots and then hangs shortly after init starts** -- check that the
management interface was brought back down before the kexec. The vendor records
the NIC DMA-ing into memory the next kernel has not initialised yet, appearing
as corruption or "Bad page state".
