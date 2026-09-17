# Building an image for the Cisco Nexus 3172TQ

Only what is specific to this board. The general build — toolchain, packages,
image, VM — is in [docs/BUILDING.md](../../../docs/BUILDING.md), and repeating
it here means it will drift.

> **These commands have not been run for this board.** The steps are the same
> ones that build the Arista 7050TX-64 — same architecture, same profile, same
> datapath package — plus one new boot backend. Nothing here is expected to be
> surprising, and nothing here is confirmed.

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
| `CONFIG_IGB` | no management interface, and nothing in `dmesg` about a network device — see [the management port](hardware.md#the-management-port), which is not fully resolved |

`CONFIG_EFIVAR_FS` is also built in. Nothing needs it to boot; it is what will
let the switch write its own firmware boot entry instead of doing it from a
console session.

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

Two files, both read off a switch running the vendor's OS, both landing in
`config/`, both gitignored:

| generator | produces | without it |
|---|---|---|
| `tools/mkportmap.sh` | `portmap.conf` — which physical lane reaches which logical port, and each PHY's MDIO address | the datapath refuses to start |
| `tools/mkpolarity.sh` | `polarity.conf` — which lanes the PCB inverts, and the per-core lane swizzle | ports link and carry zero frames, with no error at either end |

Both read the same capture, so take it once. On the switch, with root
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

**There is no `serdes.conf` or `retimer.conf` generator on this board, and that
is a real difference rather than an omission.** Every front-panel port has a
PHY in front of it — the 40G cages are retimed through BCM84328s, not
direct-attach — so nothing drives a channel off the ASIC SerDes and there are
no preemphasis or TX-FIR coefficients anywhere in the vendor's configuration to
capture. The Arista sibling needs four generators; this board needs two.
See [hardware.md](hardware.md#no-serdes-tuning).

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
complaint is indistinguishable from a corrupt file:

```sh
file out/images/cisco-n3172tq/vmlinuz
# expect: ... PE32+ executable (EFI application) x86-64
```

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
