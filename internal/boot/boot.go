// Package boot wraps a composed image in the envelope a particular bootloader
// installs from.
//
// # What is, and is not, per-bootloader
//
// The plan expected slot selection to need a backend each: GRUB counters,
// U-Boot's bootcount, Aboot's boot-config. It does not. NOSaic keeps its slot
// pointer on its own journal-less boot partition and reads it in its own
// initramfs, so choosing and rolling back a slot works identically on every
// board, and a bootloader only has to load a kernel.
//
// What genuinely differs is how an image gets onto a switch in the first
// place, and that is all a backend does here. ONIE runs a self-extracting
// shell archive; Aboot boots a SWI; a virtual board is handed a disk directly.
package boot

import (
	"fmt"
	"io"
	"sort"
)

// Image is what a backend is given: the artifacts a composed image produced.
type Image struct {
	// Kernel and Initramfs are separate files because some installers place
	// them where a bootloader can find them rather than inside the image.
	Kernel    string
	Initramfs string

	// Squashfs is the root filesystem; Disk is the whole partitioned image.
	// A backend uses whichever suits how its bootloader installs.
	Squashfs string
	Disk     string

	Board   string
	Arch    string
	Version string

	// DTB is a compiled device tree, when the board supplies one.
	//
	// x86 boards describe themselves through ACPI and need none. A PowerPC or
	// ARM board cannot boot without one, and it belongs in the FIT beside the
	// kernel rather than being deployed separately: U-Boot's `bootm addr#conf`
	// selects a configuration, and that configuration names the device tree the
	// kernel is handed. Empty means the board did not supply one.
	DTB string

	// U-Boot needs to be told things the other bootloaders work out for
	// themselves: which architecture name it uses, and where in RAM to put the
	// kernel. Those are properties of the board, so they come from board.yml.
	UBootArch  string
	UBootLoad  string
	UBootEntry string

	// UBootStage is where a downloaded image is parked in RAM before bootm
	// unpacks it. Distinct from UBootLoad, which is where the kernel inside
	// it ends up: staging an image on top of its own unpack address makes it
	// overwrite itself partway through, and the symptom is a board that
	// prints nothing further.
	UBootStage string

	// FDTAddr and RamdiskAddr place those two blobs in RAM. Empty lets U-Boot
	// choose, which is usually right.
	FDTAddr     string
	RamdiskAddr string

	// FITHash is the digest used inside the FIT. Empty means sha256; an older
	// U-Boot may know only crc32.
	FITHash string

	// Console is the board's serial console as the kernel spells it, e.g.
	// "ttyS0,115200". Needed because a U-Boot board must be handed an
	// explicit command line.
	Console string

	// KernelParams are appended to the kernel command line.
	KernelParams string

	// AbootMaxHWEpoch is the newest Arista hardware epoch this image claims to
	// support; Aboot refuses a board whose epoch is higher. Empty means 1.
	AbootMaxHWEpoch string
}

// Backend produces an installable artifact for one bootloader.
type Backend interface {
	// ID is the name a board's boot: field selects.
	ID() string

	// Describe says in one line how this bootloader installs, so `nosaic
	// catalog` and the generated per-switch pages can explain it without
	// restating it in three places.
	Describe() string

	// Tools names the host commands this backend shells out to. Declared
	// rather than discovered because the builder container is built from a
	// package list, and a tool nobody wrote down is a tool nobody installs:
	// aboot shipped without zip, and uboot without dtc, both found only when
	// CI went red. check verifies every name here is in the Dockerfile.
	Tools() []string

	// Wrap writes the installable artifact into outDir and returns its path.
	Wrap(img Image, outDir string, log io.Writer) (string, error)
}

var backends = map[string]Backend{}

func register(b Backend) { backends[b.ID()] = b }

// For returns the backend a board's boot: field names.
func For(id string) (Backend, error) {
	b, ok := backends[id]
	if !ok {
		return nil, fmt.Errorf("unknown bootloader %q (known: %s)", id, All())
	}
	return b, nil
}

// All lists the bootloaders NOSaic can install for.
func All() []string {
	out := make([]string, 0, len(backends))
	for id := range backends {
		out = append(out, id)
	}
	sort.Strings(out)
	return out
}
