# Cisco Nexus 3172TQ

The newest board in this tree, and the first that is not a whitebox.

Every board here so far was built to run somebody else's NOS: the Aristas boot
Aboot, the Edgecores boot ONIE, and in both cases the vendor built a door and
labelled it. This switch shipped with NX-OS and no door. There is no ONIE, no
installer environment, no ODM reference design to lean on, and no documentation
— so the whole of this directory came out of reverse engineering the running
machine and its firmware.

**NOSaic runs on this board.** It netboots through the vendor loader's own
TFTP to userspace, drives the fans off the ASIC die temperature, brings up all
48 copper ports and all six 40G cages, and routes: four OSPF adjacencies, two
over copper to the 7050TX-64 and two over 40G to the 7050SX2 and the AS5610,
each pinging under 1.3 ms.

Everything here was read off the hardware rather than guessed — there is no
ONIE, no installer environment, no reference design and no documentation for
this machine.

| | Where | State |
|---|---|---|
| Boot backend | [`internal/boot/uefi.go`](../../internal/boot/uefi.go) | new; written, never run against hardware |
| EFI system partition | [`internal/imgbuild/disk.go`](../../internal/imgbuild/disk.go) | new; `buildESP` |
| Kernel config | [`recipes/linux/config/x86_64.fragment`](../../recipes/linux/config/x86_64.fragment) | three symbols added, all three settled |
| Datapath | [`datapath/td2/`](../../datapath/td2/) | same driver family as the 7050TX-64, plus the BCM84328 cage bring-up the cages need |
| Platform HAL | [`internal/platformhal/n3172tq/`](../../internal/platformhal/n3172tq/) | fans, sensors, PSUs, board id from PROM, ASIC die temperature |

| | |
|---|---|
| ASIC | Broadcom **BCM56854_A2** (`14e4:b854`), Trident II, driver `BCM56850_A0` |
| Arch | **x86_64** — Intel Pentium @ 2.00 GHz, Ivy Bridge core + DH89xxCC "Cave Creek" PCH |
| Memory | 4 GB |
| Disk | **1944 MiB internal eUSB flash**, behind EHCI — not SATA |
| Front panel | **48 × 10GBASE-T + 6 × 40G QSFP+** — 54 ports, 72 ASIC logical ports |
| Management | `mgmt0` at PCI `01:00.1` (`8086:0438`, `igb`), MAC `b4:de:31:3f:a5:c0` |
| Boot | **UEFI firmware directly** → EDK2 shell → our `BOOTX64.EFI`, from disk or USB; or the vendor loader's TFTP + NBI, which is how the lab unit runs today |
| Console | `ttyS0` @ **9600** |
| Board codename | **`quickzinc2`** (`qz2`) — Cisco's, and it is how the firmware refers to this board throughout |
| Vendor OS | NX-OS 7.0(3)I7(9) |
| Status | **bringup** — boots, cools, routes; 48 copper and 6 × 40G up; netbooted, with the management VRF and VLAN/SVI support (switchapi 1.2) |

- **[Hardware reference](docs/hardware.md)** — the block diagram, the boot
  chain, the port map, the four platform transports, and the quirks
- **[Build](docs/build.md)** — building an image, and the three generators you
  have to run against your own switch
- **[Install](docs/install.md)** — getting it onto the switch, and getting back
- **[Todo](docs/todo.md)** — the ordered path from here, and it starts with the
  fans

> **Start over the network, not with an install.** The vendor loader's own TFTP
> boots an `mknbi-linux` NBI container and ours goes all the way to userspace.
> Three loader defects had to be worked around to get there — a 512-byte first
> segment, a padded tail, and a cleared EFI loader signature — and all three
> are written up in
> [install.md](docs/install.md#netbooting-use-the-loaders-tftp-not-ipxe).
> Nothing is written to the switch and nothing survives the reboot, which is
> what makes it the right way to work on this board.
>
> ⚠ **Not the embedded iPXE**, which is a different path and a dead end: it
> fetches our kernel and has no loader that will execute it.
>
> ⚠ **And do not run `ipxe` at the loader prompt.** It is a persistent
> boot-mode change with no undo from the loader; the escape is the BIOS TAB
> boot menu. That cost a recovery on 2026-09-17 and is written up in
> [install.md](docs/install.md#-do-not-run-ipxe-at-the-loader-prompt).
>
> **The fans are driven.** `nosaic platform thermal` tracks the ASIC's own die
> temperature — the hottest thing in the box, and the only sensor no i2c part
> can see — and regulates against a 55-90 °C band. The three board diodes idle
> between 31 and 38 °C while the die sits near 57, which is why regulating on
> them ran the fans at 83 % duty for no reason.

## Why this board

Three reasons, and the third is the one that made it worth the time.

**It is the same silicon NOSaic is already bringing up.** `BCM56854_A2` here,
`BCM56855` rev `0x03` on the Arista 7050TX-64 — two bins of one part, the same
A2 stepping, and the vendor's own SDK banner names the driver it binds:

```
Chip=BCM56854_A2 Rev=0x03 Driver=BCM56850_A0
```

`BCM56850_A0` is the Trident II base driver, which is the file `datapath/td2`
already uses. Both boards are 48 ports of 10GBASE-T behind BCM84848 PHYs. So
this is a new board with **no new datapath** — the axis it changes is the boot
chain, on purpose.

**It answers whether "no board knowledge in the core" survives a vendor box.**
Adding this switch touched four files outside its own directory: a boot
backend, an EFI system partition in the image builder, five lines of the
initramfs, and three kernel symbols. Nothing central lists it. That is the
claim the architecture makes, tested against hardware that was never meant to
run anything but its vendor's software.

**It is a working reference implementation of the thing we are building.** Two
metres away from the 7050TX-64, on the same generation of silicon, with a
vendor OS to compare against register by register when ours misbehaves. When
`datapath/td2` asks the SDK for a field group and gets an answer that looks
wrong, this box shows what the same chip reports when it is working: `fp show`
prints the qsets, the select codes, the slice assignment and the meter
accounting in one output. Its ingress Field Processor is **4096 entries**,
measured — twice Trident+'s, and the number `datapath/common/acl.c` will be
judged against.

## The firmware is the bootloader

This board has a vendor loader and NOSaic does not use it.

Cisco's `loader>` is a customised GNU GRUB 2, built as a PE32+ EFI application
and stored **inside the BIOS flash** — there is no EFI system partition on the
vendor's disk at all, which is why `Ctrl-L` reaches a loader prompt on a disk
that has nothing bootable on it. It keeps stock GRUB's core and replaces
everything a user would recognise, including the network stack; its additions
are namespaced `rom*`, and they include its own TFTP client, a Signature
Envelope verifier, and a loader for `mknbi-linux` NBI containers.

That last one is the problem. The loader boots **only** an NBI container, and
wrapping our kernel in one is where the attempt stopped:

- Stock `mknbi-linux 1.2-7` emits five or six segment descriptors and **the
  loader parses at most four**. Cisco's images have exactly four.
- There is an **address floor** between `0x82800` and `0x92800` — the same
  descriptor is refused below it and accepted above.
- Rebuilt in Cisco's exact two-segment shape, the container validates, the
  transfer completes, the loader prints `CardIndex = 11091` — and the board
  **resets**. A normal NX-OS boot continues past that point with `Image valid`.
  Nobody has established why.

Underneath the loader there is nothing in the way. The firmware has **no Secure
Boot machinery at all** — no Platform Key, no `db`/`dbx`, no TCG protocol, no
`SecureBootDxe`, and EDK2's no-op `SecurityStubDxe` is what ships. It is not
disabled; it was never built, and by specification it cannot be enabled without
a Platform Key there is nowhere to put. There is no hardware root of trust
either: no IOFPGA, no MIFPGA, no ACT2, all three FPGA BAR parameters reading
`0`, which is why Cisco's own trust anchor on this platform is *software*.

So `boot: uefi`. `CONFIG_EFI_STUB` makes a bzImage a valid PE32+ application,
the firmware launches it from an EFI system partition as
`\EFI\BOOT\BOOTX64.EFI`, and the NBI reset stays somebody else's puzzle.

## And it is tried before the disk is spent

The vendor loader's TFTP is the way in, and the NBI container it demands is a
packaging step rather than a format to reverse — `mknbi` is a real,
still-archived tool, and `cisco-re/tools/nbi_build.py` emits the exact
three-segment shape the loader accepts:

| segment | vtag | load | contents |
|---|---|---|---|
| 0 | 17 | `0x94000` | `bzImage[0:512]` — read as the kernel parameter block |
| 1 | 20 | `0x100000` | `bzImage[512:]` |
| 2 | 21 | `0x4000000` | the initramfs |

On the hardware that gets to:

```
Kernel loaded successfully
Loading intird 67579836
big_linux_boot
(c) Copyright 2018, Cisco Systems.     <- reset
```

Kernel in, initramfs in, our command line through, and a reset at the handoff
with no kernel output — not even `earlyprintk`. Three theories are already
dead: the exec address (the vendor's image has the same one), `setup_sects`
(the vendor's kernel has the same layout), and KASLR (`nokaslr` fails
identically). What is left is that the loader builds a 3.4-era `boot_params`:
it reads **only 512 bytes** as the parameter block, and a 6.12 setup header
runs to `0x268`.

⚠ **The embedded iPXE is a separate path and a dead end.** It fetched the boot
script, resolved its relative URIs and pulled the kernel — then
`Could not select: Exec format error`, with `imgstat` showing the image with
**no type**. No bzImage loader, no EFI image loader, and `Boot0001 "EFI
Network"` is that same iPXE out of the firmware volume rather than a PXE client
that could be given better firmware.

A FAT USB stick is the independent second route, and it bypasses the loader's
`boot_params` entirely: `vmlinuz` as `\EFI\BOOT\BOOTX64.EFI`, `initrd.img`
beside it, `startup.nsh` at the root, then **TAB** during POST → `[ 3 ] - EFI
USB Device`.

A netboot here is a **RAM boot**: the root filesystem travels inside the
initramfs, because there is no partition of ours on the disk to mount. Two
things follow, and the build enforces both rather than documenting them:

- **Nothing touches the disk**, so the vendor OS survives and a power cycle
  returns to it. That is what makes this the first thing to try.
- **Nothing survives the reboot.** No persistent data partition means a
  password or a port map set on the running switch is gone. So the `uefi`
  backend **refuses to build an installer from a RAM-boot image** — that
  installer would work, and would silently lose every setting at the next
  reboot — and **refuses to build a netboot bundle from a non-RAM-boot one**,
  which would stop in a rescue shell hunting for a disk slot that is not
  there.

This is also the only chance to probe the platform i2c bus. NX-OS has
`i2cdetect` and no `/dev/i2c-*` nodes, and its own board-controller module owns
the bus; under a netboot the bus is idle and ours, which is what a platform HAL
for this board needs and there is no other way to get it.

## One thing that is genuinely odd, and is written down loudly

The EDK2 UEFI Shell is in NOSaic's boot path on this board, permanently and on
purpose.

The EFI stub takes its command line from the firmware's LoadOptions, and a boot
entry created with `bcfg boot add` has none — so a kernel launched from a plain
boot entry gets no `console=` and no `initrd=`, which on a 9600 serial console
is a completely silent boot of a kernel with no root filesystem. The shell, on
the other hand, passes arguments and auto-runs `startup.nsh`. `Boot0002 "EFI
Internal Shell"` is already active on this firmware, so nothing has to be
enabled.

NOSaic's EFI system partition therefore carries a `startup.nsh` that finds the
partition with `\EFI\BOOT\BOOTX64.EFI` on it — iterating `fs0` through `fs3`,
because inserting a USB stick renumbers the aliases — and launches the kernel
with a full command line.

The tidy ending is a real boot entry whose optional data carries that command
line as UCS-2, written with `efibootmgr` from the running switch.
`CONFIG_EFIVAR_FS` is built in for it and
[todo.md](docs/todo.md#5-a-real-firmware-boot-entry-instead-of-the-shell)
carries it.

## Two vendor-data generators, not four — and one that reads our own switch

The Arista 7050TX-64 ships four vendor-data generators — port map, polarity,
retimer, SerDes taps — because its cages are direct-attach and every channel
has to be tuned. This board ships **two**, and the missing pair is
architectural rather than an oversight.

**Every front-panel port here has a PHY in front of it.** 1–48 are BCM84848
10GBASE-T; 49–72 are BCM84328, one per QSFP cage on the first lane of each
group of four. So the six 40G cages are *retimed*, not direct-attach, and
nothing on this board drives a channel straight off the ASIC SerDes. That is
why no preemphasis and no TX-FIR coefficients appear anywhere in the vendor's
configuration for this board — and why the SerDes library named for this
platform, `libsrdscfgn3k.so`, is a 4 KB stub whose lane accessor returns `NULL`
while its siblings for other platforms run to 178 MB. The board buys its way
out of SerDes tuning.

What you do still have to generate is the port map and the polarity data, from
one capture off your own switch:

```sh
# on the switch, with root
bcm-shell.0> config show

# on the build host
platform/cisco-n3172tq/tools/mkportmap.sh  --stdin < captured.txt > portmap.conf
platform/cisco-n3172tq/tools/mkpolarity.sh --stdin < captured.txt > polarity.conf
mv portmap.conf polarity.conf platform/cisco-n3172tq/config/
```

⚠ **Take that capture before you install.** After an install the vendor's SDK
is gone, and getting it back means putting NX-OS on the box first. It is the
one irreversible ordering constraint in the whole bring-up.

`config show` prints **568 soc properties** — 72 portmap entries plus PHY
assignment, MDI pair swaps, per-lane polarity flips and per-core lane swizzles.
It is the vendor's equivalent of our hand-maintained, unregenerable
`portmap.conf`, readable in one command, and it is the single most useful thing
this switch has given the project.

The third tool, `tools/mki2cmap.sh`, is the other way round: it reads **our
own** switch, under a netboot, and it exists because the vendor's OS is the
obstacle rather than the source. Every environmental on this board — four
sensors, the fan controller, both power supplies, the SPROM holding the thermal
thresholds — sits behind one i2c mux at `0x70` that nobody has mapped, and
NX-OS has `i2cdetect` with deliberately no `/dev/i2c-*` nodes because its own
board-controller module owns that bus. A netboot is the only moment the bus is
both reachable and idle.

The kernel and the image carry what that needs — `I2C_I801`,
`I2C_MUX_PCA954x`, `GPIOLIB` (⚠ which no x86 defconfig sets, and which the
mux driver silently needs), `HWMON`, a shortlist of sensor drivers as modules,
and busybox's `i2c*` applets. A switch that cannot read its own temperature is
one nobody should leave running, so that path ships even though the HAL that
will use it does not exist yet.

## Running it today

The lab unit is netbooted, not installed: the vendor loader TFTPs an NBI and
hands off to our kernel ([install](docs/install.md#netbooting-use-the-loaders-tftp-not-ipxe)).
Being RAM-booted, it has no data partition and no ssh key, so its
configuration is baked into the NBI at build time. Copy `network.site.conf`
and `frr.site.conf` in as `network.conf` and `frr.conf` for the build and
remove them afterwards: `make check` refuses to pass with them in the tree.
The generated `portmap.conf`, `polarity.conf` and `retimer.conf` have to be
there too, or nosd restart-loops.

As of 2026-09-25 it runs the same build as the other lab switches:
- **the management VRF** ([docs/vrf.md](../../docs/vrf.md)), mgmt0 as `eth0`
  in table 1001;
- **addresses from its own ID PROM**: `switch-mac.sh` asks `nosaic platform
  mac` for `b4:de:31:3f:a5:c0`, so the tap and SVI MACs are `02:31:3f:a5:c0:xx`
  rather than the `02:00:00:00:00:xx` it shared, index for index, with the
  7050TX-64;
- **VLANs and SVIs** (switchapi 1.2). Same td2 datapath as the 7050TX-64,
  where trunks are proven. Nothing has been driven through this board yet.

⚠ **The console server can lose this port.** Three times on 2026-09-24/25,
port 30 of the 2811 went silent: the telnet negotiation arrives and then
nothing does. It happened twice after a Ctrl-L spam into a live shell and once
during a loader catch. The cause is most likely a receive/transmit race in the
2811's `nm32a` driver, and only a cold cycle of the 2811 cleared it. At a
loader that is already interrupted, type slowly and do not spam.

## Reverse engineering

The investigation is not in this repository and must not be brought into it.

**`cisco-re/devices/n3k-3172tq`** — not public. The loader disassembled and its
29 commands recovered, the NBI format and its segment limits, the UEFI boot
policy and variable store decoded, the card-config symbol tables, the platform
transports, the TD2 FP/TCAM geometry with a CLI→TCAM correlation end to end,
the shipped CoPP policy with its 89-entry supervisor punt table itemised, and
the full session transcript.

Large binaries — the NX-OS image, the 181 MB SDK and ACL/QoS set, the seven
squashfs filesystems, the 8 MB BIOS dump read off this board's own flash — are
in **`cisco-firmware`** under `nexus3172tq/`.

No vendor SDK source is copied here and none may be. Cisco's Broadcom 6.4.8
plus 217 private patches grants neither reproduction nor derivative works;
OpenBCM does, which is why NOSaic can ship it — and OpenBCM 6.5.24 is the newer
tree anyway, post-dating almost everything Cisco backported. Read theirs by
`file:line`; do not copy from it.
