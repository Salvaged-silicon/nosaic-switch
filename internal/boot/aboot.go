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
// Both of the questions this comment used to leave open are now answered from
// two EOS SWIs pulled off a switch's flash: every member is Stored rather than
// deflated, and version is the first member. The metadata Aboot reads is in
// Wrap below.
// abootSWIVersion satisfies Aboot's "at least 4.14.7" floor. It is not a
// version of NOSaic and not a real EOS release; see the note in Wrap.
const abootSWIVersion = "4.99.0"

type aboot struct{}

func (aboot) ID() string { return "aboot" }

func (aboot) Tools() []string { return []string{"zip"} }

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

# Aboot EXPORTS swipath; it does not pass it as an argument. EOS's own boot0
# reads ${swipath} without ever assigning it, which is what settles this. The
# $0 fallback is for running this by hand during bring-up.
: "${swipath:=$0}"

echo "NOSaic %s for %s"

# Everything we need is in the archive Aboot is running us from.
rm -f /tmp/nosaic-kernel /tmp/nosaic-initrd
# kernel-params is extracted with the rest. It is inside the archive, so a
# test for it beside the archive is always false -- which is what the first dry
# run on hardware showed: the kernel booted with the console setting and
# nothing else, and the board's memmap reservation silently never arrived.
# That failure would have surfaced much later, as a datapath that could not map
# its DMA pool.
if [ -d "$swipath" ]; then
    cp "$swipath/nosaic-kernel" "$swipath/nosaic-initrd" /tmp/
    [ -f "$swipath/kernel-params" ] && cp "$swipath/kernel-params" /tmp/
else
    unzip -oq "$swipath" nosaic-kernel nosaic-initrd -d /tmp
    unzip -oq "$swipath" kernel-params -d /tmp 2>/dev/null
fi

# The slot is not chosen here. NOSaic's initramfs reads the pointer from its
# own boot partition, so an A/B decision and a rollback work the same way on
# every board and the bootloader never has to know about them.
CMDLINE="console=ttyS0,9600n8 panic=5"
if [ -f /tmp/kernel-params ]; then
    CMDLINE="$CMDLINE $(cat /tmp/kernel-params)"
fi
echo "NOSaic: cmdline: $CMDLINE"

kexec --load /tmp/nosaic-kernel \
      --initrd=/tmp/nosaic-initrd \
      --append="$CMDLINE"

# Aboot exports testonly when it was asked for a dry run. Honouring it is what
# makes "boot --testonly <url>" a dry run rather than a boot: everything up to
# here has already proved the SWI unpacks, the kernel and initrd stage, and the
# command line assembles -- and then we return to the prompt without jumping.
#
# The vendor's own boot0 does exactly this, at the same point. A SWI that
# ignores it boots for real at the moment somebody was deliberately being
# careful, which is the worst possible time to be surprised.
[ -z "${testonly}" ] || { echo "NOSaic: staged, not booting (testonly)"; exit 0; }

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
	// The board's own kernel arguments, which boot0 appends. Written as a
	// file rather than baked into boot0 so the same stage-0 serves every
	// board -- and so what a board needs is visible in the board, not buried
	// in a generated shell script.
	if img.KernelParams != "" {
		if err := os.WriteFile(filepath.Join(work, "kernel-params"),
			[]byte(img.KernelParams+"\n"), 0o644); err != nil {
			return "", err
		}
	}

	boot0 := fmt.Sprintf(abootBoot0, img.Version, img.Board)
	if err := os.WriteFile(filepath.Join(work, "boot0"), []byte(boot0), 0o755); err != nil {
		return "", err
	}
	// Read off two EOS SWIs from the flash of a real switch rather than
	// guessed. BLESSED=1 is the load-bearing one: it is what lets Aboot boot a
	// SWI with no signature, and without it this image is refused.
	//
	// SWI_MAX_HWEPOCH is the newest hardware epoch the image claims to
	// support, and Aboot rejects a board whose epoch exceeds it. Unlike the
	// U-Boot load address this has a safe default, because getting it wrong
	// means a visible refusal rather than a board that hangs.
	//
	// SWI_ARCH is deliberately absent: neither EOS SWI sets it, which is
	// proof Aboot does not require it.
	//
	// SWI_VERSION is Aboot's field with Aboot's meaning, not ours. It parses
	// the value as EOS's series.major.minor and refuses anything below 4.14.7
	// outright:
	//
	//	swi_version=0.0.0-dev ... [ 0 -lt 4 ]
	//	The SWI is too old. Please use a SWI with version of at least 4.14.7
	//
	// So it carries a number that satisfies that floor, and NOSaic's real
	// version travels in its own key beside it. Putting our version in a field
	// the bootloader parses to a different specification only looks tidy until
	// the bootloader disagrees -- which it does, on this board, before running
	// anything.
	//
	// The value is deliberately not a real EOS release: nothing should mistake
	// this image for one.
	epoch := img.AbootMaxHWEpoch
	if epoch == "" {
		epoch = "1"
	}
	version := fmt.Sprintf(
		"BLESSED=1\nSWI_VERSION=%s\nSWI_RELEASE=nosaic-%s\n"+
			"SWI_FLAVOR=DEFAULT\nSWI_VARIANT=US\nSWI_MAX_HWEPOCH=%s\n"+
			"NOSAIC_VERSION=%s\nNOSAIC_BOARD=%s\n",
		abootSWIVersion, img.Version, epoch, img.Version, img.Board)
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
	// version first, matching the member order of the vendor's own SWIs.
	members := []string{"version", "boot0", "nosaic-kernel", "nosaic-initrd"}
	if img.KernelParams != "" {
		members = append(members, "kernel-params")
	}
	cmd := exec.Command("zip", append([]string{"-0", "-q", "-X", out}, members...)...)
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
