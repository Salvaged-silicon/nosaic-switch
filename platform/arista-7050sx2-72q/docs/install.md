# Installing NOSaic on the 7050SX2-72Q

The switch boots NOSaic from its own flash. What is **not** here yet is the A/B
slot layout — this installs one image that Aboot boots, with no second slot, no
trial boot and no rollback, and no persistent state. See
[What this does not give you](#what-this-does-not-give-you).

For development, [running.md](running.md) boots over HTTP and writes nothing at
all. That is still the faster loop.

## Before you start

**Back the flash up first, and verify the backup.** The whole procedure below
is reversible, but only because the vendor's own image is still sitting on the
flash beside ours — and that is a property of this procedure, not a guarantee.

What is worth having off the box:

| | How |
|---|---|
| The whole eMMC | `dd if=/dev/mmcblk0 bs=1M \| gzip -1 \| nc <host> <port>` |
| The vendor SWI and `boot-config` | mount `/dev/mmcblk0p1` and copy them |
| The BIOS SPI, where Aboot lives | boot the vendor OS once, `flashrom -p internal -r` |

Check what you took. `gzip -t` the device image and confirm it decompresses to
exactly the size the switch reports; `unzip -t` the vendor SWI; compare every
md5 against one computed on the switch rather than trusting the transfer.

## Why this is recoverable

**Aboot is not on the eMMC.** It lives in the 16 MB BIOS SPI flash, so nothing
written to `/dev/mmcblk0` can remove it — Ctrl-C at the console always reaches
an Aboot prompt, and from there the vendor image on the flash boots with one
command. That is the property the whole procedure rests on.

The eMMC's small second partition is Arista's own fallback config store, a cpio
holding a `boot-config` pointing at an older vendor image. It is a second
safety net, and repartitioning would destroy it.

## Console

USB or RJ45 console to **ttyS0, 9600 8N1**, no flow control. Aboot prints on the
same port, so a wrong baud looks like a dead box.

## How Aboot loads an image

Aboot reads `/mnt/flash/boot-config`, a single line:

```
SWI=flash:/nosaic.swi
```

It opens that SWI — a zip — and runs the `boot0` inside it, which kexecs our
kernel. **No signing is enforced on this board.** The board's `prefdl` reports
`HwEpoch 1`, so a SWI declaring `SWI_MAX_HWEPOCH=1` is accepted; ours does.

## Installing

Each step proves one more thing than the last, and nothing before step 4 changes
what the switch does on its own.

### 1. Put the image on the flash

From the vendor OS, which has the flash mounted read-write and a known-good FAT
driver:

```sh
bash sudo wget -q -O /mnt/flash/nosaic.swi http://<build host>:8080/nosaic.swi
bash md5sum /mnt/flash/nosaic.swi
```

Compare that against the build host. A truncated download is a SWI that unzips
far enough to look plausible.

NOSaic can do this too, now that its kernel has MMC and VFAT — mount
`/dev/mmcblk0p1` and write to it. Prove that path deliberately rather than
discovering it during an install; the write test is in step 3 of the notes
below.

### 2. Dry run, from the Aboot prompt

```
Aboot# boot --testonly flash:/nosaic.swi
```

`testonly` is honoured by our `boot0`: it unzips the SWI, stages the kernel and
initrd, assembles the command line and calls `kexec --load` — then returns to
the prompt without jumping. What it proves is everything except the jump:

```
+ unzip -oq /mnt/flash/nosaic.swi nosaic-kernel nosaic-initrd -d /tmp
+ CMDLINE=... memmap=64M$0xd0000000 iomem=relaxed
+ kexec --load /tmp/nosaic-kernel --initrd=/tmp/nosaic-initrd ...
NOSaic: staged, not booting (testonly)
```

Read that command line. `kernel-params` lives *inside* the archive, and a
version of `boot0` that looked for it beside the archive silently booted without
the board's `memmap` reservation — which surfaces much later as a datapath that
cannot map its DMA pool.

### 3. Boot it for real, still without changing anything

```
Aboot# boot flash:/nosaic.swi
```

`boot-config` is untouched, so a power cycle still returns to the vendor OS.

### 4. Make it the default

```sh
cp /mnt/flash/boot-config /mnt/flash/boot-config.eos     # keep the original
echo SWI=flash:/nosaic.swi > /mnt/flash/boot-config
sync
```

The original is kept as a whole file rather than a commented-out line, so
restoring it never depends on whether Aboot's parser accepts comments.

Unmount the flash before rebooting.

## Going back

```sh
cp /mnt/flash/boot-config.eos /mnt/flash/boot-config
```

or, without changing anything, at the Aboot prompt:

```
Aboot# boot flash:/EOS-4.18.3.1F.swi
```

Aboot itself is never modified by any of this, which is what makes the switch
recoverable.

## Recovery ladder

1. Bad image → Ctrl-C at Aboot → boot the vendor SWI by name
2. Wrong `boot-config` → boot by name as above, then restore the saved file
3. Flash partition damaged → net-boot NOSaic ([running.md](running.md)),
   restore files from your backup
4. Partition table destroyed → net-boot NOSaic, `dd` the device image back
5. eMMC entirely dead → Aboot still boots; net-boot indefinitely

## What this does not give you

- **No A/B slots, no rollback.** One image, booted directly. The slot machinery
  exists and is CI-tested on the virtual platform; it is not what this installs.
- **No persistent state.** This installs a RAM-boot image, so the root overlay
  is a tmpfs: the port map, polarity and any addressing are lost on every boot
  and must be pushed again. See [running.md](running.md#4-site-configuration).
- **The vendor OS is still there**, and that is deliberate for now. It is the
  recovery path, and the eMMC has room for both.

Making the state persistent is **not** a matter of partitioning the eMMC, which
was the obvious plan until Aboot was asked how it resolves `flash:`. It matches
on the *controller*, so every partition on the eMMC competes for `/mnt/flash`
and the first one presented wins — add NOSaic's partitions and Aboot may boot
with `flash:` pointing at an ext4 slot and no image in sight. The full rule, and
the options that do work, are in
[hardware.md](hardware.md#how-aboot-resolves-flash).

The one this board should take is to keep the eMMC single-partition and put
NOSaic's state in files on it, mounted by loop. No repartitioning, no collision,
and Aboot's view of the device does not change at all.

## Aboot has no network of its own

Relevant only for the net-boot path; see
[running.md](running.md#3-give-aboot-a-network-and-boot-the-image).
