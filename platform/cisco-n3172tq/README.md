# Cisco Nexus 3172TQ

The newest board in this tree, and the first that is not a whitebox.

Every board here so far was built to run somebody else's NOS: the Aristas boot
Aboot, the Edgecores boot ONIE, and in both cases the vendor built a door and
labelled it. This switch shipped with NX-OS and no door. There is no ONIE, no
installer environment, no ODM reference design to lean on, and no documentation
— so the whole of this directory came out of reverse engineering the running
machine and its firmware.

**Nothing here has been built or booted.** NOSaic has never run on a 3172TQ.
The one attempt at running *any* of our own code on it — a mainline 6.6 kernel
with a busybox initramfs, wrapped in the vendor loader's own container format —
loaded, validated, and reset the board.

What exists is a board directory whose facts were read off the hardware rather
than guessed, and a boot path that steps around the thing that failed.

| | Where | State |
|---|---|---|
| Boot backend | [`internal/boot/uefi.go`](../../internal/boot/uefi.go) | new; written, never run against hardware |
| EFI system partition | [`internal/imgbuild/disk.go`](../../internal/imgbuild/disk.go) | new; `buildESP` |
| Kernel config | [`recipes/linux/config/x86_64.fragment`](../../recipes/linux/config/x86_64.fragment) | three symbols added, all three settled |
| Datapath | [`datapath/td2/`](../../datapath/td2/) | **unchanged** — same driver family as the 7050TX-64 |
| Platform HAL | — | none. See [todo.md](docs/todo.md#7-a-platform-hal) |

| | |
|---|---|
| ASIC | Broadcom **BCM56854_A2** (`14e4:b854`), Trident II, driver `BCM56850_A0` |
| Arch | **x86_64** — Intel Pentium @ 2.00 GHz, Ivy Bridge core + DH89xxCC "Cave Creek" PCH |
| Memory | 4 GB |
| Disk | **1944 MiB internal eUSB flash**, behind EHCI — not SATA |
| Front panel | **48 × 10GBASE-T + 6 × 40G QSFP+** — 54 ports, 72 ASIC logical ports |
| Management | `mgmt0` at PCI `01:00.1` (`8086:0438`, `igb`), MAC `b4:de:31:3f:a5:c0` |
| Boot | **UEFI firmware directly** → EDK2 shell → our `BOOTX64.EFI`, or iPXE over TFTP. No vendor bootloader in the path |
| Console | `ttyS0` @ **9600** |
| Board codename | **`quickzinc2`** (`qz2`) — Cisco's, and it is how the firmware refers to this board throughout |
| Vendor OS | NX-OS 7.0(3)I7(9) |
| Status | **planned** — not built, not booted |

- **[Hardware reference](docs/hardware.md)** — the block diagram, the boot
  chain, the port map, the four platform transports, and the quirks
- **[Build](docs/build.md)** — building an image, and the two generators you
  have to run against your own switch
- **[Install](docs/install.md)** — getting it onto the switch, and getting back
- **[Todo](docs/todo.md)** — the ordered path from here, and it starts with the
  fans

> **Start with a netboot, not an install.** `make netboot BOARD=cisco-n3172tq`
> builds a RAM-boot bundle that iPXE loads over TFTP: nothing is read from or
> written to the switch's disk, the vendor OS stays intact, and a power cycle
> returns to it. Installing replaces the vendor's partition table and the only
> NX-OS image on the chassis, so it should not be the first thing tried. See
> [install.md](docs/install.md#test-it-over-the-network-first).
>
> **And read [todo.md](docs/todo.md) before walking away from it.** NOSaic
> declares no platform HAL for this board, so it does not drive the fans.
> Measured under the vendor OS at idle: fan zone duty `0x28`, ASIC die 56 °C
> against a minor threshold of 100. So the vendor does not run them flat out
> and the margin is large — but what they do with *nobody* driving them has
> never been observed.

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
vendor OS that drives all 72 logical ports while NOSaic drives 8. When
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

## And it is tried over the network before the disk is spent

The loader's TFTP client is the fastest transfer on this box and the NBI gate
makes it useless to us. **iPXE gets around that**, and it is already on the
board — the loader embeds it and has a command to chainload it:

```sh
make netboot BOARD=cisco-n3172tq     # vmlinuz, initrd.img, nosaic.ipxe
```

```
loader> ipxe
loader> reboot
```

(`ipxe` arms the next boot rather than chainloading on the spot — its help
text is "On Reboot boot ipxe".)

iPXE speaks the ordinary Linux boot protocol, so the container format is not in
the path, and it takes a command line and a separate initrd — which is exactly
what the firmware's own boot entries cannot give us.

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
