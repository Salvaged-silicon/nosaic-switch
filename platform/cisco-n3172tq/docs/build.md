# Building an image for the Cisco Nexus 3172TQ

Only what is specific to this board. The general build — toolchain, packages,
image, VM — is in [docs/BUILDING.md](../../../docs/BUILDING.md), and repeating
it here means it will drift.

> **These commands have not been run against hardware.** The steps are the same
> ones that build the Arista 7050TX-64 — same architecture, same profile, same
> datapath package — plus one new boot backend.

## The short version

```sh
make toolchain ARCH=x86_64
make packages  ARCH=x86_64 PROFILE=minimal
make image     BOARD=cisco-n3172tq
```

Lands in `out/images/cisco-n3172tq/`, and the artifact to copy to a switch is

```
NOSaic-<version>-cisco-n3172tq.sh
```

a self-extracting installer — a shell script with a gzipped disk image appended
— run from a root shell on the box. See [install.md](install.md).

The toolchain and the base packages are shared with both Arista boards (same
architecture, same profile), so on a machine that has already built for either
of those, only the image step is new.

## Build the netboot bundle first

```sh
make netboot BOARD=cisco-n3172tq
```

Same build, plus `--ram-boot`, and it produces a different artifact:
`out/images/cisco-n3172tq/netboot/` with `vmlinuz`, `initrd.img`,
`nosaic.ipxe` and a README to serve over TFTP.

This is the one to build first. Installing on this board replaces the vendor's
partition table and the only NX-OS image on the chassis; a RAM boot touches
neither.

⚠ **But not over the network on this board.** The bundle is correct and the
embedded iPXE cannot execute it — `imgstat` shows the fetched kernel with no
type, `imgselect` gives `Exec format error`, and that iPXE *is* the firmware's
only network boot option. Measured on the hardware; see
[hardware.md](hardware.md#netbooting-is-not-possible-here). Put the same three
files on a FAT USB stick instead:
[install.md](install.md#test-it-from-a-usb-stick-instead).

⚠ **`--ram-boot` is not optional for a netboot and the build enforces it.** A
netbooted image has no disk of ours to mount, so the root filesystem has to be
inside the initramfs. Without it the transfer succeeds, the kernel starts, and
the initramfs stops in a rescue shell looking for an A/B slot on a disk that
has none — on a switch in a rack. `nosaic build` refuses rather than letting
that be discovered there.

⚠ **And the reverse.** A RAM-boot image is refused as an *installer* payload,
because that installer would work: it would boot, answer ssh, and lose every
password, port map and route at the next reboot with nothing reporting it.
Build without `--ram-boot` when you mean to install.

| you want to | build | you get |
|---|---|---|
| try it on hardware, disk untouched | `make netboot BOARD=cisco-n3172tq` | `netboot/` — on **this** board, copy it to a FAT USB stick rather than a TFTP server |
| install it | `make image BOARD=cisco-n3172tq` | `NOSaic-<ver>-cisco-n3172tq.sh` |

## What this board needs that the generic build does not

### `mkfs.vfat` and `mcopy`, in the builder container

New for this board, because it is the first whose first partition is an **EFI
system partition** rather than a small ext2 filesystem. `dosfstools` and
`mtools` are in `builder/Dockerfile.build`; if you are building with `NATIVE=1`
they have to be on your host.

mtools rather than mounting a loop device on purpose: the whole image build is
unprivileged, which is why every other filesystem here is made with `mke2fs -d`.

If the container predates this board, rebuild it:

```sh
make builder
```

Without them the image build fails at the ESP with `exec: "mkfs.vfat": executable
file not found` — which is at least loud.

### `nosd-td2` — the Trident II datapath, unchanged

This board declares `asic: td2` and the image builder resolves that to the
package providing `nosd` for it. **No new datapath.** The silicon here is
`BCM56854_A2` driven by `BCM56850_A0`, which is the same base driver as the
7050TX-64's `BCM56855` — the vendor's own SDK banner says so, so this is not an
inference of ours.

⚠ **`make packages` will not rebuild it after a source edit.** The package is
already built, the recipe is skipped, and the image silently carries the old
binary — which looks exactly like a fix that did not work. Force it:

```sh
rm -rf .cache/pkg/nosd-td2
make pkg PKG=nosd-td2 ARCH=x86_64
```

### A kernel with three things this board cannot boot without

All three are in `recipes/linux/config/x86_64.fragment` and all three are
shared with the other x86 boards, so there is nothing board-specific to enable.
They are listed here because each fails in a way that does not look like a
missing kernel symbol.

| symbol | without it |
|---|---|
| `CONFIG_EFI_STUB` | the kernel is not a PE32+ application, and the firmware reports it as not a valid image — which reads as a corrupt file |
| `CONFIG_USB_STORAGE`, `CONFIG_USB_EHCI_HCD` (**built in, not modules**) | the disk is an internal eUSB flash, so there is no root filesystem at all; a module cannot be loaded from a root that is not mounted |
| `CONFIG_IGB` | no management interface, and nothing in `dmesg` about a network device. Not in `x86_64_defconfig` although `e1000e` and `tg3` are — see [the management port](hardware.md#the-management-port) |

`CONFIG_EFIVAR_FS` is also built in. Nothing needs it to boot; it is what will
let the switch write its own firmware boot entry instead of doing it from a
console session.

### The environmental path, which is built in rather than deferred

A switch that cannot read its own temperature is a switch nobody should leave
running, so the path to this board's sensors ships in the image even though the
platform HAL that will use it does not exist yet.

| symbol | why |
|---|---|
| `CONFIG_I2C_I801=y` | ⚠ the PCH here is an **i801**, not the PIIX4 the Arista boards use. Different driver. Without it there is no platform bus at all |
| `CONFIG_I2C_MUX=y`, `CONFIG_I2C_MUX_PCA954x=y` | everything — sensors, fans, both PSUs, the SPROM — is behind one mux at `0x70`. ⚠ On x86 this driver binds to nothing by itself: there is no device tree and this board's ACPI does not mention a mux, so a HAL has to instantiate it through `new_device`. Probing needs no driver — it writes the channel select over `i2c-dev` |
| `CONFIG_GPIOLIB=y` | ⚠ **the mux driver needs it and no x86 defconfig sets it.** Found by the fragment check, not on hardware |
| `CONFIG_HWMON=y` | so a bound sensor is something to read rather than a bare address |
| `CONFIG_SENSORS_*=m`, `CONFIG_PMBUS=m` | a shortlist, as modules, because the parts are **not identified** — see below |
| busybox `i2cdetect`/`i2cget`/`i2cset`/`i2cdump`/`i2ctransfer` | the bring-up tools, in the image because there is no second chance to use them |

⚠ **The sensor drivers are a shortlist, not an identification.** This board's
four sensors are enumerated by Cisco's board controller rather than probed, so
the vendor's software never names the parts and neither does anything else we
have. They are modules precisely so that identification can proceed by binding
one and checking the reading is sane, rather than by a kernel rebuild per
guess.

### Finding out what is actually on the bus

```sh
# on the switch, under a netboot
platform/cisco-n3172tq/tools/mki2cmap.sh --yaml
```

⚠ **Under a netboot, and nowhere else.** This is the opposite of the port-map
generator: there the vendor's OS is the source, here it is the obstacle. NX-OS
*has* `i2cdetect` and deliberately no `/dev/i2c-*` nodes, because its own
board-controller module owns the bus — probing it there means two masters on
one bus while the switch is managing a failed power supply, for a table that is
free once our own kernel is running.

It writes one byte to the mux, repeatedly: the channel select, which is what a
mux is for. It scans with SMBus read-byte rather than quick-write, because a
quick-write probe is a write to every address on a bus carrying a fan
controller and two power supplies. And it leaves the mux deselected on exit
even if interrupted.

⚠ **The result is not yet something `board.yml` accepts.** `platform_hal.i2c`
and the Linux-i2c HAL behind it were written for the Edgecore AS4610 and live
on `board/edgecore-as4610-54t`. Two boards now want it, which is the argument
for landing it on `main`. Until then `--yaml` prints the block as a record of
what was measured.

### The Broadcom SDK is a build dependency and is never shipped

`openbcm` is staged for the compiler and stays out of the image, fetched and
hash-pinned rather than committed. Two things worth knowing before debugging a
strange failure:

- The SDK's own compile-line defines are captured into `sdk-defines.txt` and
  the datapath must build with that exact set. A wrong set compiles, links,
  runs, and corrupts every struct the SDK shares with us.
- ⚠ **Cisco's SDK is not a source of code here.** This box carries Broadcom
  6.4.8 plus 217 private patches, under a licence that grants neither
  reproduction nor derivative works. NOSaic builds OpenBCM 6.5.24, which is
  both newer and licensed for what we do with it. Read the vendor's, do not
  copy from it, and do not reference it anywhere but by file and line.

## The port map you have to generate

⚠ **The datapath will not start without it, and it is not in this repository.**

```
nosd-td2: no port map, so the chip would initialise and reach no front-panel cage
```

Three files, all read off a switch running the vendor's OS, all landing in
`config/`, all gitignored:

| generator | produces | without it |
|---|---|---|
| `tools/mkportmap.sh` | `portmap.conf` — which physical lane reaches which logical port, and each PHY's MDIO address | the datapath refuses to start |
| `tools/mkpolarity.sh` | `polarity.conf` — which lanes the PCB inverts, and the per-core lane swizzle | ports link and carry zero frames, with no error at either end |
| `tools/mkretimer.sh` | `retimer.conf` — the BCM84328 retimers' per-board conditioning | the cages link and run on the part's power-up defaults: a working link and an untuned one |

⚠ **`mkretimer.sh` is the odd one out and does not read the same capture.** It
talks to the vendor's SDK shell directly, one register read at a time, because
what it wants is not in `config show` — it is in the part, and only while the
cage is UP. It also has to run under NX-OS rather than against it. Its own
header has the procedure.

The first two read the same capture, so take it once. On the switch, with root
(see [install.md](install.md#getting-root-on-the-switch)):

```
bash-4.2# bcm-sdk-shell
bcm-shell.0> config show
```

Then:

```sh
platform/cisco-n3172tq/tools/mkportmap.sh  --stdin < captured.txt > portmap.conf
platform/cisco-n3172tq/tools/mkpolarity.sh --stdin < captured.txt > polarity.conf
mv portmap.conf polarity.conf platform/cisco-n3172tq/config/
```

`config show` is a show command; it writes nothing to the switch, which may be
carrying traffic while you run it.

**There is no `serdes.conf` on this board, and that is a real difference
rather than an omission.** Every front-panel port has a PHY in front of it —
the 40G cages are retimed through BCM84328s, not direct-attach — so nothing
drives a channel off the ASIC SerDes and there are no preemphasis or TX-FIR
coefficients anywhere in the vendor's configuration to capture.
See [hardware.md](hardware.md#no-serdes-tuning).

⚠ That argument was once used to say this board needed no `retimer.conf`
either, and that was wrong. The ASIC has no channel to tune, but the *retimer*
does, and its conditioning is per-PCB in exactly the way the Arista sibling's
repeater is. The Arista needs four generators; this board needs three.

### Why generated and not committed

The map is board data, but it is the *vendor's* board data: it exists nowhere
public and was read out of their running SDK. So NOSaic ships the generator and
the map stays yours. Same rule as the Arista boards.

What is committed is [`config/asic.conf`](../config/asic.conf) — the properties
that describe the ASIC as this *model* configures it rather than as this unit
is wired — and [`config/portmode.conf`](../config/portmode.conf), which says
which cages run broken out.

## Profile

`minimal`, which is s6 rather than systemd.

Not a flash-size decision: 512 MiB slots have room for `full`. It is that
`minimal` is the tier both working NOSaic boards actually run, and the 7050SX2
demonstrated the cost of declaring one that has never booted — an image that
reached "Started ospf6d" and then came up with no management, no datapath and
no console prompt, taking an OSPF adjacency down with it.

## Verifying before you install

This board earns this section: a bad image means a console session, a TFTP
server and the vendor image you hopefully saved.

**The layout fits the disk.** The internal flash is 1944 MiB and the layout is
1608, so there should be no complaint — but the failure mode if a slot is
oversized is a short write at the end of the device, which leaves a disk that
partitions correctly and boots nothing.

```sh
ls -l out/images/cisco-n3172tq/disk.img          # expect ~1608 MiB
sfdisk -l out/images/cisco-n3172tq/disk.img
```

Partition 1 must be type **`EFI System`**, not `Linux filesystem`. UEFI
enumerates by GPT type GUID, and a FAT filesystem in a partition typed `linux`
is one the firmware will not look inside — the disk mounts perfectly under
Linux and offers the firmware nothing.

**The kernel really is a PE32+ application.** If it is not, the firmware's
complaint is indistinguishable from a corrupt file.

⚠ **`file` is the wrong tool for this and says so confidently.** It reports

```
Linux kernel x86 boot executable bzImage, version 6.12.105 ...
```

on a kernel that *is* a valid EFI application, because the bzImage signature
matches before the PE one. Read the header instead:

```sh
K=out/images/cisco-n3172tq/vmlinuz
head -c2 "$K"                       # expect: MZ
python3 -c 'import sys
d=open(sys.argv[1],"rb").read()
o=int.from_bytes(d[0x3c:0x40],"little")
print(d[:2], hex(o), d[o:o+4], hex(int.from_bytes(d[o+4:o+6],"little")))' "$K"
# expect: b'MZ' 0x40 b'PE\x00\x00' 0x8664
```

`MZ` at zero, `PE\0\0` at the offset `e_lfanew` names, and machine `0x8664`
for x86-64. Measured on the built kernel: all three present.

**The EFI system partition has the three files on it**, at the exact paths the
firmware and the shell look for:

```sh
mdir -i out/images/cisco-n3172tq/disk.img@@1048576 ::/EFI/BOOT
mtype -i out/images/cisco-n3172tq/disk.img@@1048576 ::/startup.nsh
```

`\EFI\BOOT\BOOTX64.EFI` is the name UEFI's removable-media path requires,
exactly; anything else needs a boot entry created before the box can start it.
And read `startup.nsh` before trusting it — it carries the console speed and the
kernel command line, and a wrong baud there is a silent boot.

**The installer refuses a partition.** Cheap to check and it is the mistake that
destroys the way back:

```sh
./out/images/cisco-n3172tq/NOSaic-*.sh /dev/sda3
# expect: error: /dev/sda3 looks like a partition. Name the whole disk.
```
