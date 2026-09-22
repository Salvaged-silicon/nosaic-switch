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

## Before the image is worth installing

⚠ **Four files must be generated against your own switch first**, or the image
boots and the front panel does nothing useful. They are per-board vendor data,
so they are not shipped — see [build.md](build.md) for the commands and for
what each silence looks like. Briefly: no `portmap.conf` and the datapath
refuses to start; no `polarity.conf` and the cages link and carry nothing; no
`retimer.conf` or `serdes.conf` and Et51/Et52 transmit nothing while reporting
themselves up.

**Getting in afterwards.** The image ships `config/authorized_keys` if you put
one there, which is worth doing before the first install:

```sh
cp ~/.ssh/id_ed25519.pub platform/arista-7050tx-64/config/authorized_keys
```

That gives `ssh -i ~/.ssh/id_ed25519 root@<switch>`. The `admin` account has no
password and dropbear refuses empty ones, so **without a key the console is the
only way in** — and on a 9600 console that is a slow way to debug a switch that
has lost its network.

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

## Configuring it for YOUR switch

The image is built for the board MODEL and carries nothing that belongs to one
unit. Addresses, the management MAC, the OSPF router-id and which ports run a
routing protocol all live on the data partition instead:

```
/mnt/data/config/network.conf    addresses, routes, the management MAC
/mnt/data/config/frr.conf        router-id, and which ports run OSPF
/mnt/data/config/asic.conf       this switch's datapath overrides, merged
                                 property-by-property over the shipped file
```

`config/network.conf.example` and `config/frr.conf.example` in the board
directory show the shape. Copy them to the switch, fill in your own values, and
they survive an upgrade, a rollback and a reinstall -- the image never
overwrites them.

⚠ **network.conf and frr.conf used to ship in the image**, carrying one lab
switch's management address, its management MAC and its router-id. A second
switch built from the same source came up as a duplicate of the first: same MAC
on the management network, same OSPF router-id in the area. If you have an image
built before that changed, check what it is about to claim to be.

⚠ **network.conf and frr.conf are whole-file overrides**, unlike asic.conf.
A file in `/mnt/data/config/` REPLACES the shipped one rather than merging with
it, so it has to be complete. asic.conf merges per property, so it only needs
the lines that differ.

⚠ **The management MAC is in network.conf because the board will not say what
it is.** It lives in `prefdl` on an i2c SEEPROM that nothing here reads yet, so
`tg3` comes up with the unprogrammed Broadcom default and the real address has
to be stated. Two switches that both take the shipped default are two switches
with the same MAC.

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

**The console is silent, at every baud, on a box that is demonstrably running.**
Suspect the terminal server before the cable. On the Cisco 2811 used here the
console lines die *per chip* — `port n -> chip n/4`, so one bad chip takes four
consecutive panel ports — and `dmesg` names it (`chip N microcode failed`).
⚠ Re-downloading microcode at runtime is not a repair and made it worse; a
**power cycle of the terminal server** is what clears it. Prove the direction
first by having the switch write to its own console while you read the line:

```sh
# on the switch, under the vendor OS
bash (for i in 1 2 3 4 5; do echo LOOPBACK > /dev/ttyS0; sleep 2; done) &
```

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
