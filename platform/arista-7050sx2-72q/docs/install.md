# Installing NOSaic on the 7050SX2-72Q

> **Not yet done.** No NOSaic image has been installed to this switch's flash.
> Everything proven on this board so far was run from RAM — see
> **[running.md](running.md)**, which is the route you want for development and
> the one that never writes flash.
>
> This page records the flash mechanism, established from the switch's own
> flash, so that the first attempt is not also the first time anyone writes it
> down.

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

Then boot the image straight off a web server, leaving flash untouched:

```
boot http://<build host>:8080/nosaic.swi
```

Configure it at the prompt rather than setting NETIP and NETGW in boot-config,
which writes flash. A runtime address disappears at the next reboot, which is
what you want for a test.

**Wait for the link before the fetch.** The first ping after `ifconfig up`
regularly fails while a gigabit link negotiates; a script that treats that as
an error turns an expected few seconds into a failed boot. Check for a reply
rather than for the absence of a known error string -- "Network is unreachable"
came back from `sendto`, not from the packet-loss line, so a check written
against "100% packet loss" passed and the real failure arrived one step later
where it was harder to read.

Build the image with `--ram-boot` for this. Without it the initramfs looks for
on-disk slots that a net-booted board does not have and stops with
`NOSAIC-INITRAMFS-FAIL unknown slot 'a'`:

```
make image BOARD=arista-7050sx2-72q PROFILE=minimal ARGS=--ram-boot
```

## Arm the watchdog before booting anything experimental

The SCD watchdog is the recovery path: nothing punches it unless you do, so a
wedged image resets the board back into EOS on its own. **It is not armed when
NOSaic starts.** Aboot punches it during its own boot and leaves it disarmed on
handover, so a custom NOS begins with no recovery net at all -- and assuming
otherwise is how the first NOSaic boot here ended in a hung switch and a trip
to the PDU.

NOSaic drives it:

```
nosaic platform watchdog status         # register value and whether it is armed
nosaic platform watchdog arm 300000     # 5 minutes; 655350 ms is the maximum
nosaic platform release-asic
```

`arm` reads the register back and fails if it did not take. That matters more
here than it sounds: an earlier version wrote the timeout into the wrong field,
so it reported the value it was asked for, read back exactly that, and then
fired about a minute later on the value Aboot had left behind -- twice, in the
middle of releasing the ASIC. See the watchdog section in `hardware.md` for the
encoding and how it was established.

Disarm only with the console attached; it removes the automatic recovery.

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
