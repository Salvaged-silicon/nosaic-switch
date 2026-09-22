# Installing NOSaic on the AS4610-54T

Written for somebody holding the switch with a console cable and nothing else.

**Nobody has done this yet.** No image has been built for this board and none
has been installed. Every command below is derived from what the firmware on
this switch actually is — its own environment, its own capabilities, and the
image it boots today — and none of it has been run end to end. Where something
is expected rather than observed, it says so.

## Before you start

**This erases the disk, and that is safe here.** ONIE on this switch lives in
NOR flash — `mtd3`, 7 MiB, copied out at `0x1e100000` and booted by the
firmware — not on `/dev/sda`. The disk is entirely the operating system's, the
vendor's ONL partitions included. A botched install is recovered by asking for
ONIE at the boot menu; you do not need a vendor image to get back.

That is the opposite of its sibling. On the AS5610-52X, ONIE shares the disk
and writing a whole disk image removes the way back, which is why that board's
install page is built around avoiding it.

**Do not install on a switch that is carrying something.** These boxes are
often the quiet infrastructure of a lab — the thing every console cable runs
back to. Check what is plugged into it before you take it down; a 48-port
access switch is rarely doing nothing.

## 1. The console

**ttyS0 at 115200, 8N1.** Two independent sources agree: the U-Boot
environment says `baudrate=115200` and `consoledev=ttyS0`, and the kernel
command line the vendor's own FIT boots with is
`console=ttyS0,115200 onl_platform=arm-accton-as4610-54-r0`.

If a local inventory or console-server table says 9600 for this box, the table
is wrong — 9600 is the usual speed for the Arista gear in the same rack and it
is easy to copy across. At the wrong rate the switch boots perfectly and shows
you nothing but noise.

## 2. What boots this board

```
  U-Boot 2012.10  (NOR mtd0)
      │
      └─ bootcmd: run boot_diag; run check_boot_reason; run nos_bootcmd; run onie_bootcmd
                                                             │              │
                                              the installed NOS ┘              │
                                              ONIE, if that fails or is asked ─┘
```

`nos_bootcmd` is the variable the installer owns. `onie_bootcmd` is the safety
net and is never touched.

Reaching ONIE deliberately, from a running NOS:

```sh
onie-select -i -f && reboot      # install mode
onie-select -r -f && reboot      # rescue: a shell, no install attempt
```

or interrupt the countdown at the console and pick ONIE from the boot menu.

## 3. What the installer is

`make image BOARD=edgecore-as4610-54t` produces two things: a disk image, and a
self-extracting shell archive with that image inside it, which is what ONIE
runs. `internal/boot/onie.go` writes the script.

In order, it:

1. **writes the whole disk image** to `/dev/sda`, partition table and all —
   NOSaic's layout is not a variation on ONIE's, it replaces it;
2. **writes the FIT** into partition 1, raw, at a known byte offset, then reads
   the first four bytes back and checks for `d0 0d fe ed` before continuing;
3. **sets the firmware's `nos_bootcmd`** to point at that partition.

Step 3 is the one that will not work here on the first attempt, and it is worth
knowing before rather than after.

### ⚠ `fw_setenv` has nothing to write to

The installer sets `nos_bootcmd` with `fw_setenv`, which writes the U-Boot
environment through an MTD device. On this board the environment is in NOR
behind the QSPI controller, and **the QSPI node in our device tree is
disabled** — mainline has no driver for this SoC's AXI clock tree, and a QSPI
clocked wrong reads plausible rubbish out of the bootloader you rely on to
recover the switch. So there is no `/proc/mtd`, and `fw_setenv` fails.

ONIE's own environment access does work — it runs the vendor's kernel, which
has the vendor's QSPI driver — so the installer running **under ONIE** should
manage it. If it does not, set it by hand once, at the U-Boot prompt:

```
setenv nos_bootcmd 'usb start; usbiddev; setenv bootargs console=ttyS0,115200; usbboot 0x70000000 ${usbdev}:1 && bootm 0x70000000#nosaic'
saveenv
boot
```

Type it exactly, including the single quotes and the `${usbdev}` unexpanded —
see the note at the end of this page about that.

## 4. Installing

```sh
# 1. Build. The image and the installer come out together.
make image BOARD=edgecore-as4610-54t

# 2. Serve it where the switch can reach it.
python3 -m http.server 8080 --directory out/images/edgecore-as4610-54t

# 3. Get the switch into ONIE install mode, then from its prompt:
onie-nos-install http://<your-server>:8080/NOSaic-<version>-edgecore-as4610-54t.bin
```

ONIE downloads the installer, runs it, and reboots. Expect the disk write to
take a while: the image is a couple of gigabytes of mostly zeros and the disk
is behind USB 2.0.

### Trying it without writing anything first

There is a much better first move than installing, and this board supports it.
The FIT can be loaded over the network into RAM and booted, touching neither
the disk nor the environment. If it fails you power-cycle and nothing has
changed.

At the U-Boot prompt:

```
setenv autoload no
dhcp
setenv serverip <your-tftp-server>
tftpboot 0x70000000 nosaic.itb
bootm 0x70000000#nosaic
```

This is how the AS5610 was first brought up, and on a board where nothing has
ever been booted it is the right thing to do first. The FIT is in
`out/images/edgecore-as4610-54t/`.

## 5. First boot — what to watch for, in order

Nobody has seen this boot, so this is a checklist of the things most likely to
stop it, in the order they would.

| Stage | Looks like when it fails | Where to look |
|---|---|---|
| Console | garbage, or nothing | baud — 115200, not 9600 |
| `bootm` | "Bad FIT image format" or "can't get kernel image" | the FIT digest — this firmware knows only crc32 and sha1 |
| Kernel start | silence after `Starting kernel ...` | the load address, `0x61008000`; DRAM here is based at `0x61000000` |
| Console output | garbage after the kernel takes over | the UART reference clock, 62.5 MHz not 25 |
| Second CPU | boots, `nproc` says 1 | `secondary-boot-reg` — `0xffff042c` here, not mainline NSP's `0xffff0fec` |
| Root mount | "VFS: Unable to mount root fs" | **USB.** The disk is behind EHCI, and the PHY has no mainline driver |
| Management | no interface at all | `CONFIG_BGMAC_PLATFORM`, not `BGMAC_BCMA` |
| Front panel | every port down | there is no datapath yet. This is expected, not a fault |

The last row is worth repeating: **this board has no `nosd` yet.** An image
built today boots, gives you a console and a management port, and forwards
nothing. `make image` says so while building it.

## 6. Getting back

ONIE is in flash and is always reachable.

```sh
onie-select -i -f && reboot      # then install something else
```

or from ONIE itself:

```sh
onie-nos-uninstall               # removes the NOS and clears nos_bootcmd
```

To put the vendor's OS back you need its installer; Edgecore's ONIE images for
`arm-accton-as4610-54-r0` are what ONIE expects. Nothing NOSaic does prevents
that — the disk is just a disk.

## 7. One bug this family of firmware has, inherited from the AS5610

**`${usbdev}` must survive being written.** The boot command refers to a
variable that does not exist until `usbiddev` has run, so it must go into the
environment *unexpanded*. A shell or a `fw_setenv` invocation that expands it
first stores a boot command with an empty device number, and the board then
fails to find its own disk with a message about the partition rather than the
variable.

`fw_setenv nos_bootcmd '...${usbdev}...'` with single quotes, always. The
installer does this correctly; a human fixing it up at 2am often does not.

## See also

- [hardware.md](hardware.md) — the boot chain, the flash layout, what the
  firmware can and cannot do
- [build.md](build.md) — making the image this page installs
- [todo.md](todo.md) — including the two items that would remove the manual
  step in section 3
