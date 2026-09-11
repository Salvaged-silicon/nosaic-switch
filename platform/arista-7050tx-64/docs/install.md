# Installing NOSaic on the Arista 7050TX-64

Written for somebody holding the switch who has never seen it before. Assume a
console cable and nothing else.

⚠ **NOSaic has not been installed on this board yet.** What is written below is
established: the console, getting a file onto the flash, booting an unsigned
image from Aboot, and returning to the vendor OS have all been done repeatedly
with the predecessor project's images. What does **not** exist yet is the
install-to-flash step with A/B slots — see [todo.md](todo.md). Until that lands,
this board is booted as a one-shot from the Aboot prompt and returns to EOS on
the next power cycle.

## Before you start

**This does not destroy the vendor OS, and it must not.** The EOS images on
`/mnt/flash` are this board's way back, and `boot-config` is never modified —
so a plain reboot returns to EOS by itself. Booting NOSaic is an explicit act at
the Aboot prompt every time.

Check you have the vendor image before touching anything:

```sh
ls -la /mnt/flash/*.swi
```

There should be at least one `EOS-*.swi`. If there is not, stop and get one:
without it a mistake is unrecoverable without an RMA-grade recovery.

**Take a full flash backup first.** It is a few hundred megabytes and it is the
difference between an afternoon and a dead switch.

## Console

`ttyS0`, **9600** 8N1, no flow control, on the front RJ45 console port. Aboot,
EOS and NOSaic all use 9600 here; a getty at any other speed reconfigures the
port and everything printed after it is unreadable.

In this lab the console is reached over a terminal server rather than a direct
cable — a Cisco 2811 with an NM-32A, port 24, so `telnet 10.10.25.2 2024`.

## Getting the image onto the box

Aboot can load from flash, HTTP, FTP, TFTP or NFS, so there is more than one
route. The one that works with nothing but a shell on the switch:

```sh
# on the build host, serving out/images/arista-7050tx-64/
python3 -m http.server 8899 --bind <build-host-ip>

# on the switch
wget -q http://<build-host-ip>:8899/<image>.swi -O /mnt/flash/nosaic.swi.new
md5sum /mnt/flash/nosaic.swi.new
mv -f /mnt/flash/nosaic.swi.new /mnt/flash/nosaic.swi
```

Fetch to a temporary name and rename only after checking the sum: a truncated
transfer that overwrote the image in place costs a recovery session.

From EOS instead, `copy http://<host>:8899/<image>.swi flash:nosaic.swi` does
the same thing.

## Installing

Not yet implemented. The intended shape, following `arista-7050sx2-72q`: two
squashfs slots and one ext4 data image as **files** on Aboot's own FAT,
loop-mounted at boot. No repartitioning — Aboot resolves `flash:` by matching on
the storage controller, so adding partitions risks the bootloader coming up
pointing at a slot with no image in it.

Until then, boot it as a one-shot:

```
Aboot# boot --testonly flash:/nosaic.swi     # stages via kexec, does not jump
Aboot# boot flash:/nosaic.swi
```

Run the dry run first. It catches a malformed or truncated image and prints
`staged OK, not jumping` before you spend a boot on it.

To reach the Aboot prompt: reload the switch and press **Control-C** when the
banner appears. The window is short, so start sending before you expect it.

## First boot

Expect a few minutes rather than seconds: the Trident2 comes out of reset and 48
external PHYs load firmware before any copper port will link. On this board the
PHY initialisation alone takes around five minutes under the predecessor
project.

Log in as `admin`; there is no password until you set one with `passwd`.

## Going back to the vendor OS

Nothing is required — **power cycle the switch**. `boot-config` still points at
EOS, so it comes back on its own. That is the whole recovery story on this board
while NOSaic is booted as a one-shot.

If you are at the Aboot prompt and want it immediately:

```
Aboot# boot flash:/EOS-4.14.16M.swi
```

Once an installed-to-flash NOSaic exists, this section needs rewriting — at that
point the vendor images are still on the FAT but the boot path is no longer
pointing at them by default.

## When it does not work

**The banner scrolls past and EOS boots.** The Control-C window was missed.
Reload and start sending Control-C before the banner appears, not after.

**`boot` reports the image cannot be loaded.** Check the md5 against the build
host. A truncated `wget` produces exactly this.

**Aboot refuses the image on hardware-epoch grounds.** `aboot_max_hwepoch` in
`board.yml` is not yet set for this board. Read the real value from `prefdl`
under EOS and set it.

**It boots, answers on the management port, and forwards nothing.** Most likely
the image has no datapath: this board declares `asic: td2`, and if
`recipes/nosd-td2/` is not built the image builder warns and carries on. It is
also exactly the shape of image used to prove rollback, so it is easy to build by
accident.

**Copper ports show link but pass no traffic.** Two known causes on this board,
both in [hardware.md](hardware.md): the MAC interface not following the
negotiated speed, and the PHY firmware handshake failing transiently on a cold
start. Neither reports an error — check MMD `7.19` on the port and on a working
one before suspecting the cable.
