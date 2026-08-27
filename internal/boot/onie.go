package boot

import (
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

echo "NOSaic %s for %s (%s)"

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
tail -n +$SKIP "$0" | tar -xO disk.img | dd of="$DISK" bs=1M conv=fsync

sync
echo "NOSaic installed. Reboot to start it."
exit 0

__NOSAIC_PAYLOAD__
`

func (o onieSFX) Wrap(img Image, outDir string, log io.Writer) (string, error) {
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

	head := fmt.Sprintf(installer, img.Version, img.Board, img.Arch)
	if !strings.HasSuffix(head, "\n") {
		head += "\n"
	}
	if _, err := f.WriteString(head); err != nil {
		return "", err
	}

	// The payload is an uncompressed tar so the installer can stream one
	// member out of it with the tools ONIE has, without needing space for a
	// decompressed copy on a switch that may have very little.
	cmd := exec.Command("tar", "-cf", "-",
		"-C", filepath.Dir(img.Disk), filepath.Base(img.Disk))
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
