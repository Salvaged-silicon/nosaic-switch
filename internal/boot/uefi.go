package boot

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
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
if [ ! -b "$DISK" ]; then
    echo "error: $DISK is not a block device" >&2
    exit 1
fi

# ⚠ REFUSE A PARTITION.
#
# /dev/sda3 is a plausible typo for /dev/sda and it is the vendor's bootflash
# on the first board using this backend. Writing a partition table into a
# partition destroys the filesystem holding the image somebody would recover
# with, and dd will do it without comment.
case "$DISK" in
    *[0-9]) echo "error: $DISK looks like a partition. Name the whole disk." >&2; exit 1 ;;
esac

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
