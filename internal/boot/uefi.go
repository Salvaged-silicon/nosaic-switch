package boot

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

func init() { register(uefi{}) }

// uefi is for a board whose firmware boots our kernel directly, with no
// vendor bootloader in the path at all.
//
// # Why this is a backend and not "aboot without the SWI"
//
// On the Arista and ONIE boards a vendor bootloader owns the boot: Aboot
// unpacks a SWI, U-Boot reads a FIT, and NOSaic's job is to produce the
// envelope that bootloader installs from. This backend exists for the case
// where there is no such envelope, because the firmware itself is the loader:
// the kernel is built with CONFIG_EFI_STUB, which makes it a valid PE32+ EFI
// application, and UEFI launches it from the EFI system partition.
//
// The first board here is a Cisco Nexus 3172TQ, which has a vendor loader --
// a customised GRUB in the BIOS flash -- and it is deliberately stepped
// around. That loader only boots an `mknbi-linux` NBI container, and a
// container built in its exact shape loads, validates and then resets the
// board for reasons nobody has established yet. The firmware underneath it
// has no Secure Boot machinery at all (no PK, no db, no TCG protocol, no
// SecureBootDxe -- checked, rather than switched off), so the shorter path is
// to leave the vendor loader out of the chain. See
// platform/cisco-n3172tq/docs/hardware.md.
//
// # What this backend does and does not build
//
// It does not build the EFI system partition. That is a partition inside the
// disk image, so it is assembled by the image builder along with the rest of
// the layout -- see imgbuild.buildESP. What is left for a backend to do is
// the same thing every other backend does: produce the single artifact an
// operator copies to a switch in order to install.
//
// # The artifact, and why it is a shell archive
//
// A self-extracting installer, in the shape of the ONIE one, for a board that
// has no ONIE. It is run from a root shell on whatever is on the box today --
// on the Nexus, NX-OS's own `feature bash-shell` -- and it replaces the disk.
//
// That is not a workaround for the lack of an installer environment. It is the
// only environment this class of hardware reliably has: the box already runs
// a Linux with dd, and the alternative is a USB stick and physical access to a
// switch that is usually in somebody else's rack.
type uefi struct{}

func (uefi) ID() string { return "uefi" }

// mkfs.vfat and mcopy belong to the EFI system partition, which the image
// builder assembles rather than this backend. They are declared here anyway,
// because this is the only place the tool check can see them: a board booting
// this way cannot produce a bootable disk without them, and a tool nobody
// wrote down is a tool the builder container does not install.
func (uefi) Tools() []string { return []string{"tar", "mkfs.vfat", "mcopy"} }

func (uefi) Describe() string {
	return "the firmware boots the kernel itself from an EFI system partition; installed by a self-extracting installer run from a root shell on the box"
}

// installerUEFI is the script half. It runs on the switch, under whatever OS
// is still installed, and it has to assume very little: dd, tar, gunzip and
// sync. Everything a vendor OS is certain to have.
//
// Deliberately NOT modelled on ONIE's environment. There is no
// $onie_boot_dev to tell us which disk we booted from and no fw_setenv to
// point firmware at the result, so the disk is named on the command line and
// the firmware is pointed at NOSaic by hand, once, from the EFI shell. Both
// differences are in the board's install page.
const installerUEFI = `#!/bin/sh
# NOSaic installer. Generated -- do not edit.
#
# Run this from a root shell on the switch, with the target disk as its only
# argument. Everything below the marker is a tar archive appended to it.

set -e

export PATH="/usr/sbin:/sbin:/usr/bin:/bin:$PATH"

echo "NOSaic %s for %s (%s)"

DISK="$1"
if [ -z "$DISK" ]; then
    echo "usage: $0 <disk>          e.g. $0 /dev/sda" >&2
    echo "" >&2
    echo "This writes a whole partition table. Name the disk, not a partition." >&2
    exit 2
fi
# ⚠ REFUSE A PARTITION, AND DO IT BEFORE ANYTHING ELSE.
#
# /dev/sda3 is a plausible typo for /dev/sda and it is the vendor's bootflash
# on the first board using this backend. Writing a partition table into a
# partition destroys the filesystem holding the image somebody would recover
# with, and dd will do it without comment.
#
# Checked ahead of the block-device test on purpose: a mistyped partition that
# does not exist should still be told it is a partition, rather than getting
# "not a block device" and sending somebody to look for the node.
case "$DISK" in
    *[0-9]) echo "error: $DISK looks like a partition. Name the whole disk." >&2; exit 1 ;;
esac

if [ ! -b "$DISK" ]; then
    echo "error: $DISK is not a block device" >&2
    exit 1
fi

# What is about to be destroyed, and a chance to stop. Non-interactive callers
# set NOSAIC_YES=1; there is no default yes, because the default here is
# somebody's only switch.
echo ""
echo "This will ERASE $DISK completely -- partition table and all --"
echo "including the vendor OS and any image you would recover with."
echo ""
if [ "${NOSAIC_YES:-}" != "1" ]; then
    printf "Type ERASE to continue: "
    read answer
    [ "$answer" = "ERASE" ] || { echo "aborted"; exit 1; }
fi

SKIP=$(awk '/^__NOSAIC_PAYLOAD__$/ { print NR + 1; exit 0; }' "$0")

# Streamed through gunzip rather than staged on disk. The image is mostly
# zeros, and the filesystem this installer was copied onto is usually the one
# being overwritten -- so there is nowhere to stage it.
echo "writing the image to $DISK"
tail -n +$SKIP "$0" | tar -xO disk.img.gz | gunzip -c | dd of="$DISK" bs=1M conv=fsync

sync
blockdev --rereadpt "$DISK" 2>/dev/null || true

# Read back the EFI system partition's signature rather than reporting success
# on dd's exit status. A short write at the end of a device leaves a disk that
# partitions correctly and boots nothing, and this is the cheapest thing that
# distinguishes the two: an ESP formatted FAT16 carries "FAT16" at offset 54
# of its first sector.
ESP_OFFSET=%d
if ! dd if="$DISK" bs=1 skip=$((ESP_OFFSET + 54)) count=5 2>/dev/null | grep -q FAT; then
    echo "error: no FAT filesystem at offset $ESP_OFFSET; the disk did not take the image" >&2
    exit 1
fi
echo "EFI system partition verified at offset $ESP_OFFSET"

sync
echo ""
echo "NOSaic is on $DISK."
echo ""
echo "⚠ THE FIRMWARE DOES NOT KNOW ABOUT IT YET, AND WILL NOT FIND IT BY ITSELF."
echo "  This disk had no EFI system partition before now, so there is no boot"
echo "  entry pointing at one. Follow 'Pointing the firmware at NOSaic' in the"
echo "  board's install page before rebooting -- a reboot now lands at the"
echo "  vendor loader with nothing it recognises to boot."
echo ""
exit 0

__NOSAIC_PAYLOAD__
`

func (u uefi) Wrap(img Image, outDir string, log io.Writer) (string, error) {
	if img.Disk == "" {
		return "", fmt.Errorf("uefi needs a disk image")
	}
	// ⚠ REFUSED RATHER THAN BUILT, because the installer would work.
	//
	// A RAM-boot image carries its root filesystem in the initramfs and has no
	// persistent data partition. Written to a disk it boots, answers ssh and
	// looks entirely correct -- and every password, port map and route set on
	// it is gone at the next reboot, with nothing reporting that. An operator
	// would find out weeks later.
	//
	// The same image netbooted is exactly the right thing, so say so.
	if img.RAMBoot {
		return "", fmt.Errorf("a RAM-boot image has no persistent data partition and must "+
			"not be installed: nothing configured on it would survive a reboot.\n"+
			"       Netboot it instead -- the bundle is in %s/netboot/ -- or rebuild "+
			"without --ram-boot to install", outDir)
	}
	out := filepath.Join(outDir,
		fmt.Sprintf("NOSaic-%s-%s.sh", img.Version, img.Board))

	fmt.Fprintf(log, "==> building the installer\n")

	f, err := os.Create(out)
	if err != nil {
		return "", err
	}
	defer f.Close()

	// FITOffset carries the byte offset of the first partition, which for this
	// backend is the EFI system partition. Reused rather than given its own
	// field: it is the same fact -- where on the disk the thing the firmware
	// reads begins -- and the installer verifies it for the same reason the
	// ONIE one verifies the FIT.
	head := fmt.Sprintf(installerUEFI, img.Version, img.Board, img.Arch, img.FITOffset)
	if !strings.HasSuffix(head, "\n") {
		head += "\n"
	}
	if _, err := f.WriteString(head); err != nil {
		return "", err
	}

	gz, err := gzipFile(img.Disk, filepath.Join(outDir, "disk.img.gz"))
	if err != nil {
		return "", err
	}
	cmd := exec.Command("tar", "-cf", "-", "-C", filepath.Dir(gz), filepath.Base(gz))
	cmd.Stdout = f
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return "", fmt.Errorf("appending the payload: %w", err)
	}
	if err := os.Chmod(out, 0o755); err != nil {
		return "", err
	}
	return out, nil
}

// ipxeScript is what iPXE runs. It is the whole reason this bundle is a
// directory rather than two files.
//
// # Why iPXE and not the vendor loader's own TFTP
//
// Cisco's loader has a TFTP client and it works -- 5.3 MB across a subnet
// boundary at 3.7 MB/s, measured on this hardware. What it will not do is boot
// anything but an `mknbi-linux` NBI container, and our kernel in that container
// resets the board after the loader prints CardIndex.
//
// iPXE is already on the box. The loader embeds it (`grub_load_ipxe`) and has
// an `ipxe` command to chainload it on the next reboot, and the firmware's own
// `Boot0001 "EFI Network"` PXE path can load it too. It speaks the ordinary
// Linux boot protocol, so the NBI gate is not in the path at all.
//
// # Why the paths are relative
//
// iPXE resolves a relative URI against the URI of the script it is running, so
// a bundle works from whatever directory it was dropped into on whatever
// server -- no path baked in at build time, and no second place to edit when
// the server changes. The ${next-server} form is in the comment for anyone
// who needs to be explicit.
const ipxeScript = `#!ipxe
# NOSaic %s for %s. Generated -- do not edit.
#
# Serve this directory over TFTP or HTTP and boot it with either of:
#
#   loader> ipxe ; reboot        start the iPXE embedded in the BIOS flash.
#                                The command arms the NEXT boot rather than
#                                chainloading on the spot.
#   DHCP filename                point it at this file for the firmware's own
#                                "EFI Network" boot entry
#
# Paths are relative to this script's own URI. To be explicit instead, use:
#   set base tftp://${next-server}/nosaic
#   kernel ${base}/vmlinuz ...

echo NOSaic %s netboot for %s
echo .

# ⚠ initrd= ON THE KERNEL LINE AS WELL AS THE initrd COMMAND.
#
# The command loads the file; the parameter tells the kernel which loaded
# image is its initramfs. Without it a kernel with more than one loaded file
# picks wrong, and with exactly one it is merely redundant -- so it is always
# written.
kernel vmlinuz initrd=initrd.img console=%s,%dn8 %s
initrd initrd.img
boot || goto failed

:failed
echo NOSaic: iPXE could not boot the image. Check that vmlinuz and initrd.img
echo are in the same directory as this script and are readable over TFTP.
shell
`

// netbootREADME travels with the bundle because the bundle travels: it is
// copied onto a TFTP server, usually not by whoever built it.
const netbootREADME = `NOSaic %s netboot bundle for %s
================================================================

  vmlinuz      the kernel. Also a valid PE32+ EFI application.
  initrd.img   the initramfs, WITH THE ROOT FILESYSTEM INSIDE IT.
  nosaic.ipxe  the iPXE script: what to load, and the command line.

This is a RAM boot. The root filesystem is inside initrd.img, so nothing is
read from or written to the switch's disk and the vendor OS is untouched.

⚠ NOTHING SURVIVES THE REBOOT. There is no persistent data partition on a RAM
boot, so a password, a port map or a route set on the running switch is gone
when it restarts. That is what makes this safe to try and useless to keep.

To serve it
-----------
Drop all three files in one directory on a TFTP server the switch can reach,
then either:

  * catch the loader prompt, run ipxe and then reboot -- that starts the iPXE
    already in this board's BIOS flash, on the next boot rather than at once;
    or
  * point the DHCP filename option at nosaic.ipxe and let the firmware's own
    "EFI Network" boot entry fetch it.

The loader keeps its own IP configuration in CMOS, independent of anything the
OS sets:

  loader> show ip
  loader> show gw
  loader> set ip <addr> <mask>
  loader> set gw <addr>

⚠ The loader autoboots about two seconds after its prompt appears. Catching the
prompt and sending the command have to happen on one connection.
`

// Netboot writes the bundle an operator serves over TFTP.
//
// Separate from Wrap because it answers a different question. Wrap produces the
// thing that replaces the switch's disk; this produces the thing that proves
// the image first, on a board where the first of those is not reversible
// without the vendor image.
func (u uefi) Netboot(img Image, outDir string, log io.Writer) (string, error) {
	if img.Kernel == "" || img.Initramfs == "" {
		return "", fmt.Errorf("a netboot bundle needs a kernel and an initramfs")
	}
	// ⚠ REFUSED WITHOUT --ram-boot, AND THIS IS THE GUARD THAT EARNS ITS KEEP.
	//
	// A netbooted image has no disk of ours to mount. Built without an
	// embedded root filesystem, the initramfs comes up, looks for an A/B slot
	// on a disk that has none, and stops in a rescue shell with "unknown
	// slot" -- on a switch in a rack, after a transfer that appeared to
	// succeed. The Makefile carries a comment about exactly this having
	// happened once, which is reason enough to make it impossible rather than
	// documented.
	if !img.RAMBoot {
		return "", fmt.Errorf("a netboot image must carry its root filesystem in the " +
			"initramfs, or it will boot to a rescue shell looking for a disk slot " +
			"that does not exist.\n       Rebuild with --ram-boot")
	}

	dir := filepath.Join(outDir, "netboot")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	fmt.Fprintf(log, "==> building the netboot bundle\n")

	for _, f := range [][2]string{
		{img.Kernel, "vmlinuz"},
		{img.Initramfs, "initrd.img"},
	} {
		if err := copyFileTo(f[0], filepath.Join(dir, f[1])); err != nil {
			return "", err
		}
	}

	consoleDev, consoleBaud := "ttyS0", 115200
	if img.Console != "" {
		// Console arrives as the kernel spells it, "ttyS0,9600".
		if dev, baud, ok := splitConsole(img.Console); ok {
			consoleDev, consoleBaud = dev, baud
		}
	}
	script := fmt.Sprintf(ipxeScript, img.Version, img.Board, img.Version, img.Board,
		consoleDev, consoleBaud, img.KernelParams)
	if err := os.WriteFile(filepath.Join(dir, "nosaic.ipxe"), []byte(script), 0o644); err != nil {
		return "", err
	}
	readme := fmt.Sprintf(netbootREADME, img.Version, img.Board)
	if err := os.WriteFile(filepath.Join(dir, "README"), []byte(readme), 0o644); err != nil {
		return "", err
	}
	return dir, nil
}

// splitConsole takes "ttyS0,9600" apart. The kernel's spelling is one string
// and iPXE wants it rebuilt with the 8N1 suffix, so it has to come apart.
func splitConsole(s string) (dev string, baud int, ok bool) {
	i := strings.IndexByte(s, ',')
	if i < 0 {
		return s, 115200, true
	}
	n, err := strconv.Atoi(s[i+1:])
	if err != nil {
		return "", 0, false
	}
	return s[:i], n, true
}

func copyFileTo(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()
	if _, err := io.Copy(out, in); err != nil {
		return err
	}
	return out.Sync()
}
