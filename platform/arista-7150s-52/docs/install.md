# Installing NOSaic on an Arista DCS-7150S-52

**There is nothing to install yet.** This board is `planned`: no image has been
built for it and no NOSaic kernel has booted on it. This page exists so that the
access details are written down where the next person looks, and it will become
an install procedure when there is one to describe.

What follows is how to get a console and power on the lab unit, and what is
known about the boot path an install will use.

## Before you start

`/mnt/flash` is 1.5 GB with **511 MB free**. EOS is 416 MB of what is used and
must stay — it is the only way back, and the last EOS release that supports this
platform. Most of the rest is reverse-engineering trace data, worth roughly
another half gigabyte if an install ever needs it.

Installing any NOS over EOS on this chassis is **not reversible without the
vendor image**. EOS 4.16.8M is the last version that supports this platform —
the FM6000 boards were dropped after 4.23 — so take a copy of `/mnt/flash`,
including the `.swi` and `boot-config`, before touching anything. There is no
download page to fall back on.

## Getting a console

`ttyS0` at **9600 8N1**. Not 115200 — that is the AS5610, and sweeping this
line's speed to find out is specifically the thing that has made it silent
before.

On the lab bench the unit is panel 4 on the Cisco 2811 console server:

```sh
telnet 10.10.25.2 2003          # 9600 8N1; expect: sw7150-lab login:
```

Power is **outlet 6 on `apc1`** (`10.10.24.2`), named `7150S-unitA`. Check the
outlet name before switching anything: `apc1` carries the whole rack.

```sh
# on apc1
olStatus 6
```

Given that this board's hardware reset does not work (see
[hardware.md](hardware.md)), the power controller is the recovery path, not a
convenience.

## The boot path an install will use

Aboot 2.1.0 "norcal2", reading `boot-config` from the FAT32 `/mnt/flash` on the
USB DOM (`/dev/sda1`):

```
SWI=flash:/EOS-4.16.8M.swi
```

Aboot's `Control-C` shell needs no password, which is the recovery path that
does not depend on EOS credentials.

Aboot here boots **unsigned** SWIs, the same as on the 7050SX2, so a NOSaic SWI
is a matter of putting it on the flash and pointing `boot-config` at it. Catch
Aboot at the console to get back to a known image.

## What has to be true before this page can be written properly

- a NOSaic kernel boots on this chassis and reaches a console prompt;
- the management NIC comes up with the board's real MAC (it is in prefdl, not in
  the NIC — and it is *not* what you read after a kexec from EOS);
- there is a way back. On a board whose hardware reset hangs, "reboot into the
  other slot" is an assumption that has to be demonstrated before an A/B install
  means anything.
