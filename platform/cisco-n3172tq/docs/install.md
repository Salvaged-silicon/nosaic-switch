# Installing NOSaic on a Cisco Nexus 3172TQ

Written for somebody holding the switch. Assume a console cable and nothing
else.

> **NOTHING BELOW HAS BEEN DONE ON THIS BOARD.** NOSaic has never booted on a
> 3172TQ. Every step is built out of a mechanism that *was* exercised on this
> hardware — the recovery shell, the loader's TFTP transfer, the EFI shell, the
> UEFI boot policy — but the sequence as a whole is unproven, and the board's
> status is `planned` for exactly that reason. Read
> [hardware.md](hardware.md) first. If you are the first person through here,
> [todo.md](todo.md) is the order to do it in.

## Before you start

**This erases the switch completely.** NOSaic replaces the vendor's MBR
partition table with its own GPT layout, which destroys `bootflash:` and with
it the NX-OS image. It is not reversible without that image.

⚠ **There is exactly one NX-OS image on the chassis.** `sda1`, the 24 MiB
partition that could hold a second, is empty apart from `lost+found` — it was
formatted on 2018-03-20 and never used. So there is no spare copy on the box.

**Get the image off the switch first, and check it.** From NX-OS:

```
switch# dir bootflash: | include bin
   568308736    Mar 20 2018 00:00:00  n3100-compact.7.0.3.I7.9.bin
switch# show file bootflash:n3100-compact.7.0.3.I7.9.bin md5sum
```

Copy it somewhere off the box and verify the md5 matches. Then also save the
things that are not in the image:

```
switch# show running-config
switch# show license usage
switch# show inventory
switch# show environment
```

`show inventory` is worth keeping because it records the serials, and on this
family the PSU serial is the only way to identify which supply failed.

**What is *not* destroyed.** The BIOS and the vendor loader are in the BIOS
flash, not on the disk, so both survive an install untouched — `Ctrl-L` still
reaches `loader>` on a switch with a blank disk. That is what makes recovery
possible at all, and it is the single most useful fact on this page.

## Console

| | |
|---|---|
| Connector | RJ45 marked **CONSOLE** on the front panel |
| Device | `ttyS0` |
| Speed | **9600**, 8N1, no flow control |

⚠ **9600, not 115200.** The Edgecore AS4610 in the same lab is 115200 and the
two switches look the same from the front. At the wrong speed the BIOS banner
is garbage, which reads as a dead board.

In this lab: front-panel port **31** on the console server, `telnet 10.10.25.2
2030`. Power is `apc2` (`10.10.38.2`) **outlet 3**.

## Getting root on the switch

Two ways in, and you want the first one.

### From NX-OS, if you have credentials

⚠ **`feature bash-shell` is a running-config setting and does not survive a
reload.** It was enabled during the reverse-engineering session and the lab
unit lost it to a power cycle, so `run bash` comes back as
`Syntax error while parsing 'run bash ...'` — which reads like a missing
command rather than a disabled feature. Check with
`show feature | include bash` and turn it back on:

```
switch# configure terminal
switch(config)# feature bash-shell
switch(config)# end
switch# run bash sudo su
bash-4.2#
```

⚠ **`mgmt0` is in the `management` network namespace.** Anything from this
shell that needs the network must be wrapped, or it fails with no useful error
— it simply returns non-zero, because the default namespace has no route
anywhere:

```
bash-4.2# ip netns exec management tftp -p -l file -r file 10.22.1.5
```

### From the loader, if you do not

This works on a switch with no credentials at all, and it is not the documented
recovery procedure — the documented kickstart-only recovery does **not** work
here, because since 7.0(3)I2(1) the N3000 family uses a single image binary.

1. Power-cycle the switch and hold **Ctrl-L** while the BIOS runs. You get
   `loader>`.
2. Set the undocumented recovery flag and boot:

   ```
   loader> cmdline recoverymode=1
   loader> boot bootflash:/n3100-compact.7.0.3.I7.9.bin
   ```

3. You land at `switch(boot)#`. Then:

   ```
   switch(boot)# start
   bash-4.2#
   ```

`recoverymode` is not a loader feature — `cmdline` simply appends to the kernel
command line, and NX-OS's own init interprets the flag later. Which is useful
to know: **any** kernel parameter can be passed that way.

⚠ **The loader autoboots about two seconds after the prompt appears.** If you
are scripting this, catching the prompt and sending the command have to happen
on one connection. And if your console server replays a backlog on connect, it
will hand you a stale `loader>` from a previous boot that matches instantly and
fires your command into a running NX-OS. Match only a prompt that arrives
*after* this boot's BIOS banner.

## Test it over the network first

⚠ **Do this before the install, not after it.** Installing replaces the
vendor's partition table and the only NX-OS image on the chassis. A netboot
touches neither: the root filesystem travels inside the initramfs, nothing is
read from or written to the disk, and a power cycle returns the switch to
NX-OS.

Build the bundle:

```sh
make netboot BOARD=cisco-n3172tq
```

Three files land in `out/images/cisco-n3172tq/netboot/` — `vmlinuz`,
`initrd.img` and `nosaic.ipxe`, with a README beside them. Drop all three into
one directory on a TFTP server the switch can reach.

Set the loader's IP configuration. It keeps this in CMOS, independent of
anything NX-OS or NOSaic configures, so it may already be right:

```
loader> show ip
loader> show gw
loader> set ip 10.10.39.2 255.255.255.0
loader> set gw 10.10.39.1
```

Then start the iPXE that is already in this board's BIOS flash. ⚠ Like
`efi_shell`, this command arms the *next* boot rather than chainloading
immediately — its own help text is "On Reboot boot ipxe":

```
loader> ipxe
loader> reboot
```

iPXE will DHCP, fetch `nosaic.ipxe`, and boot the kernel with a full command
line. If your DHCP server does not hand out a `filename`, give iPXE the script
directly at its prompt:

```
iPXE> dhcp
iPXE> chain tftp://10.22.1.5/nosaic/nosaic.ipxe
```

⚠ **Why iPXE and not `boot tftp://`.** The loader's own TFTP client works — it
is the fastest transfer on this box — but it boots only an `mknbi-linux` NBI
container, and our kernel in one resets the board. iPXE speaks the ordinary
Linux boot protocol and takes a command line and a separate initrd, which is
what the firmware's own boot entries cannot give us.

If the loader prompt cannot be caught, `Boot0001 "EFI Network"` is already
active and reaches the same place: point DHCP's `filename` at `nosaic.ipxe`.

### What to check while it is up

⚠ **NOTHING HERE SURVIVES THE REBOOT.** There is no persistent data partition
on a RAM boot, so treat the session as read-only: anything configured is gone
when it restarts. That is the point — it is safe to try and useless to keep.

This is the cheapest opportunity to settle the things that are still open on
this board, and several of them are much harder to answer once NX-OS is gone:

```sh
# 1. The disk is behind USB, not SATA. This is the single most likely first
#    failure and it is why USB_STORAGE is built in rather than a module.
cat /proc/partitions           # expect sda, 1990656 blocks, with sda1..sda6
                               # -- the VENDOR's layout, untouched

# 2. The management port. igb should bind TWO 8086:0438 functions and only one
#    is the front panel. Identify it by MAC, not by name.
ip -o link | grep -i b4:de:31:3f:a5:c0
ls /sys/bus/pci/devices/0000:01:00.1/net/

# 3. The fans, which is the item that should stop you walking away. Under the
#    vendor OS this chassis idles at fan zone duty 0x28 with the ASIC die at
#    56 C. NOSaic drives the fans not at all, so what they do here is whatever
#    the hardware leaves them at -- and that has never been observed.
#    Listen to it, and watch the die temperature climb or not.

# 4. The platform i2c bus, which no NOSaic board has reached on this hardware
#    and which cannot be probed from NX-OS -- it has i2cdetect but no
#    /dev/i2c-* nodes, and its own driver owns the bus. Here the bus is idle
#    and ours.
i2cdetect -l                   # expect the i801 SMBus adapter
mki2cmap.sh --yaml             # walk the mux at 0x70, channel by channel
```

That last one is worth the session on its own: the mux channel-to-device map is
what a platform HAL for this board needs, and a netboot is the only time the
bus is both reachable and not in use by somebody else's driver.
`tools/mki2cmap.sh` ships in the board directory and the `i2c*` applets ship in
the image for exactly this. Keep its output — see
[todo.md](todo.md#7-a-platform-hal).

## Getting the image onto the box

You need `NOSaic-<version>-cisco-n3172tq.sh` — see [build.md](build.md) — on
the switch's own filesystem. It is a self-extracting installer: a shell script
with a gzipped disk image appended.

The disk is 1944 MiB and the installer is a compressed image of a 1608 MiB
layout that is mostly zeros, so it is small — but it still has to live
somewhere while it runs, and the filesystem holding it is the one being
overwritten. Two options:

**A USB stick**, which is the one to prefer. The chassis has USB ports and the
running kernel mounts them:

```
bash-4.2# mkdir -p /mnt/usb && mount /dev/sdb1 /mnt/usb
bash-4.2# ls -l /mnt/usb/NOSaic-*.sh
```

**Over the network into `/bootflash`**, if you have no stick. Remember the
namespace:

```
bash-4.2# cd /bootflash
bash-4.2# ip netns exec management tftp -g -r NOSaic-0.1.0-cisco-n3172tq.sh 10.22.1.5
bash-4.2# chmod +x NOSaic-0.1.0-cisco-n3172tq.sh
```

⚠ **Do not put it on `/bootflash` if that is also where your only copy of the
NX-OS image is.** The installer overwrites the whole disk, `/bootflash`
included. If the image is still on there, copy it off first.

## Installing

```
bash-4.2# ./NOSaic-0.1.0-cisco-n3172tq.sh /dev/sda
```

The disk is named explicitly and the installer refuses anything that looks like
a partition — `/dev/sda3` is a plausible typo and it is the vendor's
`bootflash:`.

What you should see:

```
NOSaic 0.1.0 for cisco-n3172tq (x86_64)

This will ERASE /dev/sda completely -- partition table and all --
including the vendor OS and any image you would recover with.

Type ERASE to continue: ERASE
writing the image to /dev/sda
1608+0 records in
1608+0 records out
EFI system partition verified at offset 1048576

NOSaic is on /dev/sda.

⚠ THE FIRMWARE DOES NOT KNOW ABOUT IT YET, AND WILL NOT FIND IT BY ITSELF.
```

The last line is not boilerplate. **Do not reboot yet.**

## Pointing the firmware at NOSaic

This disk had no EFI system partition before now, so the firmware has no boot
entry pointing at one. A reboot at this point lands at the vendor loader with
nothing it recognises to boot.

The path through is the EDK2 UEFI Shell, which is already `Boot0002` and
already active.

1. Reboot, catch the loader with **Ctrl-L**, and tell it to come up in the
   shell next time:

   ```
   loader> efi_shell
   loader> reboot
   ```

2. You should land at `Shell>`. Check that the firmware can see our partition:

   ```
   Shell> map
   ```

   Look for a `Removable HardDisk` whose device path contains
   `Pci(0x1D,0x0)/USB` — that is the internal flash. It will have an alias,
   usually `fs0`.

3. NOSaic's EFI system partition carries a `startup.nsh`, and the shell
   auto-runs one. If it did, you will have seen

   ```
   NOSaic 0.1.0
   booting from fs0:
   ```

   and the kernel will already be coming up. **If it did not**, run it by
   hand — this always works:

   ```
   Shell> fs0:
   fs0:\> startup.nsh
   ```

   ⚠ Whether the shell finds `startup.nsh` by itself on this firmware has not
   been confirmed. If it does not, the arrangement in step 5 is what you need.

4. Confirm it boots. See [First boot](#first-boot).

5. Make it permanent. The shell's boot order is what gets you here, so leave
   the shell ahead of the vendor loader:

   ```
   Shell> bcfg boot dump -v
   Shell> bcfg boot mv 02 00
   ```

   That moves `Boot0002` (the shell) to the front, so every boot reaches the
   shell, the shell runs `startup.nsh` off our partition, and the script
   launches the kernel. It costs the shell's few-second delay on every boot.

   ⚠ **Do not use `bcfg boot add` to point straight at `\EFI\BOOT\BOOTX64.EFI`
   instead.** It works, in that the kernel starts — and the entry carries no
   optional data, so the kernel gets no command line: no `console=`, no
   `initrd=`. On a 9600 serial console that is a completely silent boot of a
   kernel with no root filesystem. The shell exists in this path precisely
   because it passes arguments.

   The tidy version of step 5 — a real boot entry whose optional data carries
   the command line as UCS-2, written with `efibootmgr` from the running switch
   — is in [todo.md](todo.md). `CONFIG_EFIVAR_FS` is built in for it.

## First boot

Roughly what to expect:

```
NOSaic 0.1.0
booting from fs0:
EFI stub: Loaded initrd from command line option
[    0.000000] Linux version 6.12.x ...
[    2.xxxxxx] usb 1-1: new high-speed USB device
[    2.xxxxxx] sd 0:0:0:0: [sda] 3981312 512-byte logical blocks
NOSAIC-INITRAMFS slot a
NOSAIC-INITRAMFS handing over to /sbin/init
...
nosaic login:
```

Log in as `admin`. **There is no password until you set one:**

```
nosaic# passwd
```

Then check what came up:

```
nosaic# nosaic show caps
nosaic# nosaic show ports
```

⚠ **`nosd` will not start yet, and it will say why.** The datapath needs a port
map generated from a switch running the vendor's OS, and until you have done
that it reports itself unconfigured rather than guessing. See
[build.md](build.md#the-port-map-you-have-to-generate).

⚠ **Check the fans.** NOSaic does not drive them — there is no platform HAL
for this board yet — so they sit at whatever the hardware leaves them at, and
that has never been observed.

For comparison, measured on this chassis under the vendor OS at idle:

| | |
|---|---|
| Fan zone 1 duty | `0x28` |
| ASIC die | 56 °C (minor 100, major 110) |
| Front-Left (D1) | 38 °C (minor 60, major 70) |
| Front-Right (D2) | 37 °C (minor 56, major 70) |
| Back (D3) | 31 °C (minor 46, major 70) |

So the vendor does **not** run these fans flat out, and the margin at idle is
large. That is reassuring and it is not an answer: the die was at 56 °C with no
datapath running, and under NOSaic with `nosd` up it will be hotter. Listen to
it, keep the first sessions short and watched, and do not leave it running
unattended until somebody has a curve. First item in [todo.md](todo.md).

## Going back to the vendor OS

Needed, and needing it means something has already gone wrong. It is possible
because **the BIOS and the vendor loader are in the BIOS flash, not on the
disk** — a blank disk still gives you `loader>`.

You need the NX-OS image you saved at the start, and a TFTP server the switch
can reach.

1. Set the loader's own IP configuration. It keeps this in CMOS, independent of
   anything NX-OS or NOSaic configures:

   ```
   loader> set ip 10.10.39.2 255.255.255.0
   loader> set gw 10.10.39.1
   loader> show ip
   loader> show gw
   ```

2. Boot the image over the network. This transfer is proven on this hardware —
   5.3 MB across a subnet boundary at 3.7 MB/s:

   ```
   loader> boot tftp://10.22.1.5/n3100-compact.7.0.3.I7.9.bin
   ```

3. NX-OS comes up running from the network image. Copy the image to the disk
   and let the vendor installer rebuild its own partitions:

   ```
   switch# copy tftp://10.22.1.5/n3100-compact.7.0.3.I7.9.bin bootflash: vrf management
   switch# install all nxos bootflash:n3100-compact.7.0.3.I7.9.bin
   ```

   `install all` is what re-creates the MBR layout and `bootflash:`. NOSaic's
   GPT table is overwritten in the process.

4. Move the vendor loader back to the front of the boot order, if you moved the
   shell ahead of it:

   ```
   loader> reboot        # then Ctrl-L into the EFI shell via efi_shell
   Shell> bcfg boot dump -v
   Shell> bcfg boot mv <shell-index> 02
   ```

   Or simply `bcfg boot mv 00 02` from wherever the shell ended up, until
   `Boot0000 "EFI Payload"` is first again.

**If you have no image and no TFTP server**, `usb1:` and `usb2:` are accepted
devices for the loader's `boot` command, and `Boot0003 "EFI USB Device"` is an
active UEFI boot entry — so a FAT stick works too.

## When it does not work

Symptom first.

**Garbage on the console from power-on.** Wrong baud. It is 9600.

**`Ctrl-L` never gets a `loader>`.** The window is short and it is during the
BIOS, before any banner you would recognise. Hold it from the moment you
power on. If a console server is replaying a backlog, what you match may be
from a previous boot.

**The loader prompt appears and your command does not take.** It autoboots
about two seconds later. Catching the prompt and sending the command have to
be one connection.

**`loader> boot ...` says `invalid magic number expected 0x1b031336`.** You
handed it something that is not an `mknbi-linux` container. The vendor loader
boots only NX-OS images. It cannot boot NOSaic and it is not meant to — see
[hardware.md](hardware.md#the-vendor-loader-and-why-it-is-not-in-our-path).

**`error: Load Address Range Check Failed`.** Also the loader, also an NBI
container problem, and also not something to fix — NOSaic does not use that
path.

**The loader prints `CardIndex = 11091` and the board resets.** That is the
known dead end in the NBI route. A normal NX-OS boot continues with `Image
valid`; nothing of ours has ever got there. You are on the wrong path; use the
EFI shell.

**`Shell> map` shows no `fs` alias for the internal disk.** The firmware found
no filesystem it recognises. Either the install did not take, or the first
partition is not typed as an EFI system partition — the installer verifies the
FAT signature at the ESP offset and fails loudly if it is absent, so a clean
install run rules the second out. Try `mount blk<n> fs0` to force it, and check
`blk` device paths for `Pci(0x1D,0x0)/USB`.

**The kernel starts and then nothing.** The most likely cause is no command
line: you got here through a plain `Boot####` entry rather than through
`startup.nsh`, so there is no `console=` and the boot is happening silently
with no initrd. Go back to [Pointing the firmware at
NOSaic](#pointing-the-firmware-at-nosaic).

**`Waiting for root device` or a kernel panic about no root filesystem.** The
disk is behind USB, not SATA. Check the kernel has `USB_STORAGE` and
`USB_EHCI_HCD` built in — not as modules, because a module cannot be loaded
from a root that is not mounted.

**No management interface, and nothing in `dmesg` about a network device.**
`CONFIG_IGB=y` is missing. The DH8900CC GbE is not covered by
`x86_64_defconfig` although `e1000e` and `tg3` are.

**Two igb interfaces, and only one works.** Expected. `01:00.1` is the front
panel and `01:00.2` is a second `8086:0438` that goes nowhere; `igb` binds
both. Pick the one with MAC `b4:de:31:3f:a5:c0` — the kernel name depends on
probe order and `mgmt0` is the vendor's name for it, not the kernel's.

**A `dmesg` line about `8086:0436` with no driver.** Not a problem. Those two
functions are `DH8900CC Null Device` — placeholders the PCH exposes that report
an Ethernet class code and are not ports. Nothing should claim them.

**Ports do not appear at all.** `nosd` has no port map. That is expected until
you generate one; see [build.md](build.md#the-port-map-you-have-to-generate).

**A port links and carries nothing, or reports another port's link state.** A
wrong port map or wrong PHY MDIO address. Both are generated data and both fail
this way rather than by erroring — the MDIO addresses on this board are
**swapped in pairs**, and physical lane numbering **skips 17–20**.
