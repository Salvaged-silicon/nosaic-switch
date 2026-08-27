package boot

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
)

func init() { register(aboot{}) }

// aboot produces the SWI that Arista's bootloader boots.
//
// # How this was determined
//
// The mechanism was read off a switch rather than assumed. A SWI is a zip
// stored uncompressed, containing an entry point named boot0 that Aboot
// executes; boot0 extracts a kernel and an initrd from the same archive and
// kexecs them. Which SWI to boot comes from boot-config in flash, a single
// line of the form:
//
//	SWI=flash:/EOS-4.14.16M.swi
//
// Only the interface is taken from that: the names Aboot looks for and the
// fact that it kexecs. The boot0 below is written from scratch and shares no
// code with the vendor's.
//
// Still to confirm on hardware: whether Aboot requires the archive be stored
// rather than deflated (the vendor's is stored, so this one is too), and
// whether it cares about the other members EOS ships. Neither can be settled
// without a switch, and both are recorded here rather than assumed away.
type aboot struct{}

func (aboot) ID() string { return "aboot" }

func (aboot) Describe() string {
	return "a SWI booted by Aboot: copy to flash and point boot-config at it"
}

// abootBoot0 is the entry point Aboot runs.
//
// It kexecs rather than continuing to boot, because that is what Aboot's
// stage-0 does: Aboot is running its own kernel by this point, and the only
// way to reach ours is to replace it.
const abootBoot0 = `#!/bin/sh
# NOSaic stage 0. Generated -- do not edit.
#
# Aboot runs this from inside the SWI. Its job is to get our kernel running,
# which means kexec: Aboot is already running a kernel of its own.

set -e

swipath="$1"
[ -n "$swipath" ] || swipath="$(pwd)"

echo "NOSaic %s for %s"

# Everything we need is in the archive Aboot is running us from.
rm -f /tmp/nosaic-kernel /tmp/nosaic-initrd
if [ -d "$swipath" ]; then
    cp "$swipath/nosaic-kernel" "$swipath/nosaic-initrd" /tmp/
else
    unzip -oq "$swipath" nosaic-kernel nosaic-initrd -d /tmp
fi

# The slot is not chosen here. NOSaic's initramfs reads the pointer from its
# own boot partition, so an A/B decision and a rollback work the same way on
# every board and the bootloader never has to know about them.
CMDLINE="console=ttyS0,9600n8 panic=5"
if [ -f "$swipath/kernel-params" ]; then
    CMDLINE="$CMDLINE $(cat "$swipath/kernel-params")"
fi

kexec --load /tmp/nosaic-kernel \
      --initrd=/tmp/nosaic-initrd \
      --append="$CMDLINE"
sync
kexec --exec
`

func (a aboot) Wrap(img Image, outDir string, log io.Writer) (string, error) {
	if img.Kernel == "" || img.Initramfs == "" {
		return "", fmt.Errorf("aboot needs a kernel and an initramfs")
	}
	fmt.Fprintf(log, "==> building the Aboot SWI\n")

	work, err := os.MkdirTemp("", "nosaic-swi-")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(work)

	files := map[string]string{
		"nosaic-kernel": img.Kernel,
		"nosaic-initrd": img.Initramfs,
	}
	for name, src := range files {
		b, err := os.ReadFile(src)
		if err != nil {
			return "", err
		}
		if err := os.WriteFile(filepath.Join(work, name), b, 0o644); err != nil {
			return "", err
		}
	}
	boot0 := fmt.Sprintf(abootBoot0, img.Version, img.Board)
	if err := os.WriteFile(filepath.Join(work, "boot0"), []byte(boot0), 0o755); err != nil {
		return "", err
	}
	version := fmt.Sprintf("SWI_VERSION=%s\nSWI_VARIANT=NOSaic\nNOSAIC_BOARD=%s\n",
		img.Version, img.Board)
	if err := os.WriteFile(filepath.Join(work, "version"), []byte(version), 0o644); err != nil {
		return "", err
	}

	out, err := filepath.Abs(filepath.Join(outDir,
		fmt.Sprintf("NOSaic-%s-%s.swi", img.Version, img.Board)))
	if err != nil {
		return "", err
	}
	_ = os.Remove(out)

	// -0 stores rather than deflates. The vendor's own SWI is stored, and
	// since Aboot has to read boot0 out of this before anything else runs,
	// matching what is known to work is worth more than the space.
	cmd := exec.Command("zip", "-0", "-q", "-X", out,
		"boot0", "version", "nosaic-kernel", "nosaic-initrd")
	cmd.Dir = work
	if b, err := cmd.CombinedOutput(); err != nil {
		return "", fmt.Errorf("building the SWI: %v\n%s", err, b)
	}

	// The line that tells Aboot to boot it. Written alongside rather than
	// inside, because it lives in flash, not in the image.
	bc := filepath.Join(outDir, "boot-config")
	if err := os.WriteFile(bc,
		[]byte(fmt.Sprintf("SWI=flash:/%s\n", filepath.Base(out))), 0o644); err != nil {
		return "", err
	}
	fmt.Fprintf(log, "    %s\n    %s (copy both to flash)\n", out, bc)
	return out, nil
}
