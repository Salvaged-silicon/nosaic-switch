package boot

import (
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

func init() { register(onieSFX{}) }

// onieSFX produces the self-extracting installer ONIE runs.
//
// ONIE fetches a single file and executes it, so the artifact is a shell
// script with a payload appended: the script finds where its own text ends,
// unpacks the rest, and writes the image to the disk. That shape is fixed by
// ONIE and is why this is not simply a tarball.
type onieSFX struct{}

func (onieSFX) ID() string { return "onie-sfx" }

func (onieSFX) Tools() []string { return []string{"tar"} }

func (onieSFX) Describe() string {
	return "a self-extracting installer run by ONIE: onie-nos-install <file>"
}

// installer is the script half. It runs on the switch, under ONIE, with the
// payload appended to it.
//
// Deliberately plain shell using only what ONIE provides. An installer that
// needs anything not already on the box is an installer that cannot run in the
// situation it exists for.
const installer = `#!/bin/sh
# NOSaic installer. Generated -- do not edit.
#
# ONIE executes this file directly. Everything below the marker is a tar
# archive appended to this script.

set -e

# ONIE sets PATH=/usr/bin:/bin and nothing else, so fdisk, mke2fs, fw_setenv
# and reboot are all absent from it. Every one of those failures reads as
# "not found" for a tool that is plainly installed.
export PATH="/usr/sbin:/sbin:/usr/bin:/bin:$PATH"

echo "NOSaic %s for %s (%s)"

# SINGLE-QUOTED, and that is load bearing.
#
# The boot command contains ${usbdev}, which U-Boot expands at boot time and
# this shell must not expand now: usbdev is unset under ONIE, so double quotes
# here would write "usbboot 0x10000000 :1" into the firmware and produce a
# switch that installs perfectly and then boots nothing. newnos/BOOT.md escapes
# every $ for the same reason; quoting once, here, is harder to get wrong than
# escaping at every use.
FIT_NAME='%s'
FIT_OFFSET=%d
NOS_BOOTCMD='%s'

# Where to write. ONIE tells us which device it booted from; falling back to
# the first disk is a guess, so it says so rather than silently choosing.
if [ -n "$onie_boot_dev" ]; then
    DISK=$(echo "$onie_boot_dev" | sed 's/[0-9]*$//')
elif [ -b /dev/sda ]; then
    DISK=/dev/sda
    echo "note: ONIE did not say which disk it booted from; assuming $DISK"
elif [ -b /dev/mmcblk0 ]; then
    DISK=/dev/mmcblk0
    echo "note: ONIE did not say which disk it booted from; assuming $DISK"
else
    echo "error: no disk found to install onto" >&2
    exit 1
fi

echo "installing onto $DISK"

# Find where the payload starts. Computed from this script's own length rather
# than hard-coded, so editing a comment above cannot corrupt the offset.
SKIP=$(awk '/^__NOSAIC_PAYLOAD__$/ { print NR + 1; exit 0; }' "$0")

# Write the whole disk image, partition table and all. NOSaic's layout is not
# a variation on ONIE's -- it has its own boot, A/B and data partitions -- so
# installing means replacing the layout, not fitting inside one.
#
# Compressed, and decompressed straight into dd rather than onto disk first.
#
# ONIE downloads this whole file into a tmpfs before running it -- /tmp on this
# board is 1012 MiB of RAM -- so the installer's size is bounded by memory, not
# by flash. An uncompressed 1.3 GiB image dies about two thirds of the way in
# with "wget: short write", which reads like a network fault and is not one. The
# image is mostly zeros, so gzip takes it to well under a tenth of that, and
# streaming through gunzip means the box never needs room for both.
tail -n +$SKIP "$0" | tar -xO disk.img.gz | gunzip -c | dd of="$DISK" bs=1M conv=fsync

sync

# The FIT, into the raw partition the firmware loads it from.
#
# Only for firmware that reads a partition rather than a filesystem. U-Boot on
# these boards has no GPT support and no interest in our layout: it is told an
# address and a partition number, reads that many bytes, and boots what it
# finds. So the image goes in whole, at the start of partition 1.
if [ -n "$FIT_NAME" ]; then
    # WRITTEN AT AN ABSOLUTE OFFSET, NOT TO /dev/sdX1.
    #
    # Straight after dd'ing a partition table the kernel is still describing
    # the layout that was there before, and ONIE's busybox has no partprobe.
    # The partition node therefore still exists and still points at the
    # PREVIOUS owner's offset -- so a dd to /dev/sda1 writes to the old first
    # partition's start, which lands somewhere in the middle of the new one.
    # It is the worst shape of bug: every command succeeds, the installer
    # reports success, and the firmware then reads zeros where the boot image
    # should be. Seen on the AS5610, where EdgeNOS's p1 began at sector 8192
    # and ours begins at 2048.
    #
    # The offset comes from the table this image was built with, so it does not
    # depend on the kernel having noticed anything.
    sync
    blockdev --rereadpt "$DISK" 2>/dev/null || true
    sleep 2
    echo "writing the boot image at offset $FIT_OFFSET"
    tail -n +$SKIP "$0" | tar -xO "$FIT_NAME" \
        | dd of="$DISK" bs=512 seek=$((FIT_OFFSET / 512)) conv=fsync,notrunc
    sync
    # Read it back from the same absolute offset. A FIT begins d0 0d fe ed, and
    # if that is not there the switch will fall through to ONIE on next boot
    # with nothing to say why.
    if ! dd if="$DISK" bs=1 skip=$FIT_OFFSET count=4 2>/dev/null | od -x | head -1 | grep -qi "d00d"; then
        echo "error: the boot image is not at offset $FIT_OFFSET after writing it" >&2
        exit 1
    fi
    echo "boot image verified at $FIT_OFFSET"
fi

# Tell the firmware where the NOS is now.
#
# ONIE boards keep a NOS boot command in the U-Boot environment, and it still
# describes the previous NOS -- on this hardware, a different partition number
# and a FIT configuration name that our image does not contain. Leaving it
# alone installs an image the box will not boot.
#
# Written last, so a failure earlier leaves the firmware still pointing at
# whatever was working before.
if [ -n "$NOS_BOOTCMD" ]; then
    if command -v fw_setenv >/dev/null 2>&1; then
        echo "setting nos_bootcmd"
        # ONIE's fw_setenv asks "Proceed with update [N/y]?" and waits. In a
        # non-interactive installer that either blocks or eats stdin, and the
        # variable silently does not get written -- so the box installs
        # successfully and then boots ONIE again.
        echo y | fw_setenv nos_bootcmd "$NOS_BOOTCMD" || {
            echo "error: could not set nos_bootcmd; this switch will not boot NOSaic" >&2
            exit 1
        }
        # Leave install mode. U-Boot boots ONIE whenever onie_boot_reason is
        # set, whatever nos_bootcmd says, so a switch that installs perfectly
        # and never clears this comes straight back to the installer. An empty
        # value deletes the variable.
        echo y | fw_setenv onie_boot_reason "" 2>/dev/null || true
        # Read it back. fw_setenv can report success and write nothing when the
        # prompt consumed the value, and the next evidence would otherwise be a
        # switch that boots the wrong thing.
        if ! fw_printenv nos_bootcmd 2>/dev/null | grep -q "usbboot"; then
            echo "error: nos_bootcmd did not stick; not rebooting" >&2
            exit 1
        fi
    else
        echo "error: fw_setenv is missing, so the firmware cannot be pointed at NOSaic" >&2
        exit 1
    fi
fi

# ONIE's own exec_installer calls reboot without a path, so on a board where
# /sbin is not in ONIE's PATH that call fails, ONIE logs a failure for an
# install that succeeded, and its error handler re-runs the step that resets
# the NOS boot command. Rebooting here, with a full path, gets in first.
echo "NOSaic installed."
if [ -x /sbin/reboot ]; then
    echo "rebooting"
    /sbin/reboot || true
else
    echo "reboot this switch to start it"
fi
exit 0

__NOSAIC_PAYLOAD__
`

const installerX86 = `#!/bin/sh
# NOSaic x86_64 ONIE installer. Generated -- do not edit.
#
# ONIE executes this file directly. Everything below the marker is a tar
# archive appended to this script containing vmlinuz, initramfs.cpio.gz,
# and rootfs.sqsh.

set -e

export PATH="/usr/sbin:/sbin:/usr/bin:/bin:$PATH"

echo "NOSaic %s for %s (%s)"

BOOT_MIB=%d
SLOT_MIB=%d
KERNEL_PARAMS='%s'

# Locate ONIE-BOOT so we preserve GRUB-BOOT and ONIE-BOOT intact.
ONIE_DEV=$(blkid -L ONIE-BOOT 2>/dev/null || findfs LABEL=ONIE-BOOT 2>/dev/null || true)
if [ -n "$ONIE_DEV" ]; then
    DISK=$(echo "$ONIE_DEV" | sed -E 's/p?[0-9]+$//')
    ONIE_PART=$(echo "$ONIE_DEV" | sed -E 's/^.*[^0-9]([0-9]+)$/\1/')
elif [ -n "$onie_boot_dev" ]; then
    DISK=$(echo "$onie_boot_dev" | sed -E 's/p?[0-9]+$//')
    ONIE_PART=$(echo "$onie_boot_dev" | sed -E 's/^.*[^0-9]([0-9]+)$/\1/')
    ONIE_DEV="$onie_boot_dev"
elif [ -b /dev/sda ]; then
    DISK=/dev/sda
    ONIE_PART=2
    ONIE_DEV=/dev/sda2
    echo "note: ONIE-BOOT not found by label; assuming $ONIE_DEV on $DISK"
else
    echo "error: no disk found to install onto" >&2
    exit 1
fi

case "$DISK" in
    *nvme*|*mmcblk*) PART_PREFIX="${DISK}p" ;;
    *)               PART_PREFIX="$DISK" ;;
esac

echo "installing onto $DISK (preserving partitions 1..$ONIE_PART)"

# Remove any previous NOS partitions after ONIE-BOOT and create NOSaic's
# boot, A/B slot, and data partitions.
if command -v sgdisk >/dev/null 2>&1; then
    for p in $(sgdisk -p "$DISK" 2>/dev/null | awk '/^ *[0-9]+ /{print $1}'); do
        if [ "$p" -gt "$ONIE_PART" ]; then
            sgdisk -d "$p" "$DISK" >/dev/null
        fi
    done
    P_BOOT=$((ONIE_PART + 1))
    P_SLOTA=$((ONIE_PART + 2))
    P_SLOTB=$((ONIE_PART + 3))
    P_DATA=$((ONIE_PART + 4))
    sgdisk -n "${P_BOOT}:0:+${BOOT_MIB}M"  -t "${P_BOOT}:8300"  -c "${P_BOOT}:nosaic-boot" \
           -n "${P_SLOTA}:0:+${SLOT_MIB}M" -t "${P_SLOTA}:8300" -c "${P_SLOTA}:nosaic-slot-a" \
           -n "${P_SLOTB}:0:+${SLOT_MIB}M" -t "${P_SLOTB}:8300" -c "${P_SLOTB}:nosaic-slot-b" \
           -n "${P_DATA}:0:0"              -t "${P_DATA}:8300"  -c "${P_DATA}:nosaic-data" \
           "$DISK" >/dev/null
elif command -v parted >/dev/null 2>&1; then
    for p in $(parted -ms "$DISK" print 2>/dev/null | awk -F: '/^[0-9]+:/{print $1}'); do
        if [ "$p" -gt "$ONIE_PART" ]; then
            parted -s "$DISK" rm "$p"
        fi
    done
    START_MB=$(parted -ms "$DISK" unit MiB print | awk -F: -v p="$ONIE_PART" '$1==p{gsub(/MiB/,"",$3); printf "%%d", $3 + 1}')
    END_BOOT=$((START_MB + BOOT_MIB))
    END_SLOTA=$((END_BOOT + SLOT_MIB))
    END_SLOTB=$((END_SLOTA + SLOT_MIB))
    P_BOOT=$((ONIE_PART + 1))
    P_SLOTA=$((ONIE_PART + 2))
    P_SLOTB=$((ONIE_PART + 3))
    P_DATA=$((ONIE_PART + 4))
    parted -s -a optimal "$DISK" \
        mkpart nosaic-boot ext2 "${START_MB}MiB" "${END_BOOT}MiB" \
        mkpart nosaic-slot-a "${END_BOOT}MiB" "${END_SLOTA}MiB" \
        mkpart nosaic-slot-b "${END_SLOTA}MiB" "${END_SLOTB}MiB" \
        mkpart nosaic-data ext4 "${END_SLOTB}MiB" 100%%
else
    echo "error: neither sgdisk nor parted is available in ONIE" >&2
    exit 1
fi

sync
partprobe "$DISK" 2>/dev/null || blockdev --rereadpt "$DISK" 2>/dev/null || true
sleep 2

DEV_BOOT="${PART_PREFIX}${P_BOOT}"
DEV_SLOTA="${PART_PREFIX}${P_SLOTA}"
DEV_DATA="${PART_PREFIX}${P_DATA}"

SKIP=$(awk '/^__NOSAIC_PAYLOAD__$/ { print NR + 1; exit 0; }' "$0")

echo "formatting and populating nosaic-boot ($DEV_BOOT)"
mke2fs -q -t ext2 -L nosaic-boot -U 8f3a1c22-0000-4000-8000-6e6f73616962 -F "$DEV_BOOT"
MNT_BOOT=$(mktemp -d /tmp/nosaic-boot.XXXXXX)
mount -t ext2 "$DEV_BOOT" "$MNT_BOOT"
mkdir -p "$MNT_BOOT/boot"
tail -n +$SKIP "$0" | tar -xO vmlinuz > "$MNT_BOOT/boot/vmlinuz"
tail -n +$SKIP "$0" | tar -xO initramfs.cpio.gz > "$MNT_BOOT/boot/initramfs.cpio.gz"
printf 'a\n' > "$MNT_BOOT/boot/active"
sync
umount "$MNT_BOOT"
rmdir "$MNT_BOOT"

echo "writing nosaic-slot-a ($DEV_SLOTA)"
tail -n +$SKIP "$0" | tar -xO rootfs.sqsh | dd of="$DEV_SLOTA" bs=1M conv=fsync

echo "formatting nosaic-data ($DEV_DATA)"
mke2fs -q -t ext4 -L nosaic-data -U 8f3a1c22-0000-4000-8000-6e6f73616963 -F "$DEV_DATA"
MNT_DATA=$(mktemp -d /tmp/nosaic-data.XXXXXX)
mount -t ext4 "$DEV_DATA" "$MNT_DATA"
mkdir -p "$MNT_DATA/config" "$MNT_DATA/secrets" "$MNT_DATA/slot-a/upper" "$MNT_DATA/slot-a/work" "$MNT_DATA/slot-b/upper" "$MNT_DATA/slot-b/work" "$MNT_DATA/log"
chmod 0700 "$MNT_DATA/secrets"
sync
umount "$MNT_DATA"
rmdir "$MNT_DATA"

echo "configuring GRUB on ONIE-BOOT ($ONIE_DEV)"
MNT_ONIE=$(mktemp -d /tmp/nosaic-onie.XXXXXX)
mount "$ONIE_DEV" "$MNT_ONIE"
GRUB_DIR=""
for d in "$MNT_ONIE/grub" "$MNT_ONIE/boot/grub"; do
    if [ -d "$d" ]; then
        GRUB_DIR="$d"
        break
    fi
done
if [ -z "$GRUB_DIR" ]; then
    GRUB_DIR="$MNT_ONIE/grub"
    mkdir -p "$GRUB_DIR"
fi
GRUB_REL="${GRUB_DIR#$MNT_ONIE}"
if [ -f "$GRUB_DIR/grub.cfg" ] && [ ! -f "$GRUB_DIR/grub.cfg.onie" ]; then
    cp "$GRUB_DIR/grub.cfg" "$GRUB_DIR/grub.cfg.onie"
fi
cat > "$GRUB_DIR/grub.cfg" <<EOF
serial --port=0x3f8 --speed=115200 --word=8 --parity=no --stop=1
terminal_input serial
terminal_output serial
set timeout=5
set default=0

menuentry "NOSaic" {
    insmod part_gpt
    insmod ext2
    search --no-floppy --label --set=root nosaic-boot
    linux /boot/vmlinuz ${KERNEL_PARAMS}
    initrd /boot/initramfs.cpio.gz
}
EOF
if [ -f "$GRUB_DIR/grub.cfg.onie" ]; then
    cat >> "$GRUB_DIR/grub.cfg" <<EOF

menuentry "ONIE" {
    configfile ${GRUB_REL}/grub.cfg.onie
}
EOF
fi
sync
umount "$MNT_ONIE"
rmdir "$MNT_ONIE"

if command -v onie-boot-mode >/dev/null 2>&1; then
    onie-boot-mode -o none 2>/dev/null || true
fi

echo "NOSaic installed."
if [ -x /sbin/reboot ]; then
    echo "rebooting"
    /sbin/reboot || true
else
    echo "reboot this switch to start it"
fi
exit 0

__NOSAIC_PAYLOAD__
`

func (o onieSFX) Wrap(img Image, outDir string, log io.Writer) (string, error) {
	if img.Arch == "x86_64" {
		return o.wrapX86(img, outDir, log)
	}
	if img.Disk == "" {
		return "", fmt.Errorf("onie-sfx needs a disk image")
	}
	out := filepath.Join(outDir,
		fmt.Sprintf("NOSaic-%s-%s.bin", img.Version, img.Board))

	fmt.Fprintf(log, "==> building the ONIE installer\n")

	f, err := os.Create(out)
	if err != nil {
		return "", err
	}
	defer f.Close()

	fitName := ""
	if img.FIT != "" {
		fitName = filepath.Base(img.FIT)
	}
	// A single quote in either value would end the quoting that makes this
	// safe. Neither has ever contained one, and a build that fails here is
	// better than a switch that boots nothing.
	if strings.ContainsRune(fitName, '\'') || strings.ContainsRune(img.NOSBootCmd, '\'') {
		return "", fmt.Errorf("a single quote in the boot image name or boot command would break the installer's quoting")
	}
	head := fmt.Sprintf(installer, img.Version, img.Board, img.Arch,
		fitName, img.FITOffset, img.NOSBootCmd)
	if !strings.HasSuffix(head, "\n") {
		head += "\n"
	}
	if _, err := f.WriteString(head); err != nil {
		return "", err
	}

	// The disk image is gzipped; the tar around it is not.
	//
	// The tar stays uncompressed so the installer can stream one member out of
	// it with the tools ONIE has. The disk image inside is compressed because
	// ONIE downloads this entire file into a tmpfs before running it, so its
	// size is bounded by the switch's RAM -- and the image is mostly zeros.
	gz, err := gzipFile(img.Disk, filepath.Join(outDir, "disk.img.gz"))
	if err != nil {
		return "", err
	}
	args := []string{"-cf", "-", "-C", filepath.Dir(gz), filepath.Base(gz)}
	if img.FIT != "" {
		// A second member rather than a second file: the installer is one
		// artifact an operator copies to a switch, and a boot image that
		// travels separately is a boot image that arrives mismatched.
		args = append(args, "-C", filepath.Dir(img.FIT), filepath.Base(img.FIT))
	}
	cmd := exec.Command("tar", args...)
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

// wrapX86 builds an x86_64 ONIE self-extracting installer that preserves
// GRUB-BOOT and ONIE-BOOT, creates nosaic-boot/slot-a/slot-b/data, and
// configures GRUB2 on ONIE-BOOT.
func (o onieSFX) wrapX86(img Image, outDir string, log io.Writer) (string, error) {
	if img.Kernel == "" || img.Initramfs == "" || img.Squashfs == "" {
		return "", fmt.Errorf("x86_64 onie-sfx needs kernel, initramfs, and squashfs")
	}
	if strings.ContainsRune(img.KernelParams, '\'') {
		return "", fmt.Errorf("a single quote in kernel_params would break the installer's quoting")
	}
	bootMiB, slotMiB := img.BootMiB, img.SlotMiB
	if bootMiB <= 0 {
		bootMiB = 64
	}
	if slotMiB <= 0 {
		slotMiB = 512
	}

	out := filepath.Join(outDir,
		fmt.Sprintf("NOSaic-%s-%s.bin", img.Version, img.Board))
	fmt.Fprintf(log, "==> building the x86_64 ONIE installer\n")

	f, err := os.Create(out)
	if err != nil {
		return "", err
	}
	defer f.Close()

	// GRUB2 expands $var inside unquoted linux command-line arguments, so a
	// literal '$' (such as memmap=64M$0xb0000000) must be escaped as '\$'.
	grubKernelParams := strings.ReplaceAll(img.KernelParams, "$", `\$`)
	head := fmt.Sprintf(installerX86, img.Version, img.Board, img.Arch,
		bootMiB, slotMiB, grubKernelParams)
	if !strings.HasSuffix(head, "\n") {
		head += "\n"
	}
	if _, err := f.WriteString(head); err != nil {
		return "", err
	}

	stage, err := os.MkdirTemp("", "nosaic-onie-x86-*")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(stage)

	for _, pair := range []struct{ src, dst string }{
		{img.Kernel, "vmlinuz"},
		{img.Initramfs, "initramfs.cpio.gz"},
		{img.Squashfs, "rootfs.sqsh"},
	} {
		dst := filepath.Join(stage, pair.dst)
		if err := os.Link(pair.src, dst); err != nil {
			if err := copyFile(pair.src, dst); err != nil {
				return "", err
			}
		}
	}

	cmd := exec.Command("tar", "-cf", "-", "-C", stage, "vmlinuz", "initramfs.cpio.gz", "rootfs.sqsh")
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

func copyFile(src, dst string) error {
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
	return out.Close()
}

// gzipFile compresses src to dst and returns dst.
//
// Done here rather than by shelling out to gzip so the build does not depend on
// which gzip is installed: the switch needs busybox gunzip to read it, and
// every gzip writes something busybox can read, but only this way is the
// compression level ours to choose.
func gzipFile(src, dst string) (string, error) {
	in, err := os.Open(src)
	if err != nil {
		return "", err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return "", err
	}
	defer out.Close()

	// Best compression: this runs once at build time and the result is copied
	// into a switch's memory, where every megabyte is one it might not have.
	zw, err := gzip.NewWriterLevel(out, gzip.BestCompression)
	if err != nil {
		return "", err
	}
	if _, err := io.Copy(zw, in); err != nil {
		return "", err
	}
	if err := zw.Close(); err != nil {
		return "", err
	}
	return dst, out.Sync()
}
