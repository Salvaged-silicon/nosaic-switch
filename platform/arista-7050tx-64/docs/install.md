# Installing NOSaic on the Arista 7050TX-64

Written for somebody holding the switch who has never seen it before. Assume a
console cable and nothing else.

**NOSaic has been installed and booted on this board.** First boot 2026-09-11,
into slot A with a persistent data image; the procedure below is a transcript of
what was done rather than a plan.

⚠ It is still booted as a **one-shot from the Aboot prompt**. `boot-config` is
untouched, so a power cycle returns to EOS by itself. Making NOSaic the default
is one line and it is deliberately not done yet — see [todo.md](todo.md).

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

Three files on Aboot's own FAT, loop-mounted at boot. No repartitioning — Aboot
resolves `flash:` by matching on the storage controller, so adding partitions
risks the bootloader coming up pointing at a slot with no image in it.

| file | what it is | where it comes from |
|---|---|---|
| `nosaic.swi` | kernel and initramfs | `NOSaic-*-arista-7050tx-64.swi` |
| `nosaic-slot-a.sqsh` | the root filesystem for slot A | `rootfs.sqsh`, renamed |
| `nosaic-data.img` | ext4: configuration, the boot pointer, each slot's writable layer | made on the switch, once |

⚠ **WITHOUT `nosaic-data.img` THE SWITCH BOOTS STATELESS AND SAYS SO ONCE.**

The initramfs falls back to a tmpfs and prints a single
`no data partition; booting stateless` line in the middle of the boot. Everything
then works and nothing is kept: no `/mnt/data/config` for a port map, no boot
pointer, so no A/B and no rollback. The first NOSaic boot on this board ran that
way for want of one file nobody had made.

It is not produced by the build — the build's `disk.img` is a whole partitioned
disk, which is the layout this board does not use — so make it once, on the
switch, from the vendor OS:

```sh
bash sudo dd if=/dev/zero of=/mnt/flash/nosaic-data.img bs=1M count=512
bash sudo mkfs.ext4 -q -F -L nosaic-data /mnt/flash/nosaic-data.img
```

512 MiB against 1.2 GB free, alongside two EOS images that must stay. It
survives reinstalls: only make it again if you mean to discard the switch's
state.

Then the two image files, fetched to temporary names and renamed only after the
sums check out:

```sh
bash sudo wget -q -O /mnt/flash/nosaic.swi.new http://<build-host>:8899/NOSaic-0.1.0-arista-7050tx-64.swi
bash sudo wget -q -O /mnt/flash/nosaic-slot-a.sqsh.new http://<build-host>:8899/rootfs.sqsh
bash md5sum /mnt/flash/nosaic.swi.new /mnt/flash/nosaic-slot-a.sqsh.new
bash sudo mv -f /mnt/flash/nosaic.swi.new /mnt/flash/nosaic.swi
bash sudo mv -f /mnt/flash/nosaic-slot-a.sqsh.new /mnt/flash/nosaic-slot-a.sqsh
bash sync
```

## Booting it

```
Aboot# boot --testonly flash:/nosaic.swi     # stages via kexec, does not jump
Aboot# boot flash:/nosaic.swi
```

Run the dry run first. It catches a malformed or truncated image and prints
`NOSaic: staged, not booting (testonly)` before you spend a boot on it. Read the
command line it echoes: `kernel-params` lives *inside* the archive, and the
`memmap=64M$0xd0000000 iomem=relaxed` reservation the datapath's DMA pool needs
has to be on that line.

A good boot says, in this order:

```
NOSAIC-INITRAMFS flash appeared after 2s (/dev/sda1)
NOSAIC-INITRAMFS data image mounted (/mnt/flash/nosaic-data.img)
NOSAIC-BOOT want slot a, active=a trial=none tries=0
NOSAIC-INITRAMFS overlay assembled (persistent=yes)
NOSAIC-NET using /etc/nosaic/network.conf
thermal: floor 60%, 25°C->40°C maps 60%->100%, every 10s
```

`persistent=yes` and the data-image line are the two worth checking; both are
absent on a stateless boot and nothing later complains.

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
