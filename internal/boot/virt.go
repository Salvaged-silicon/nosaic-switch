package boot

import (
	"fmt"
	"io"
	"path/filepath"
)

func init() { register(virt{}) }

// virt is the virtual board's "installer", which installs nothing.
//
// QEMU is handed the kernel, the initramfs and the disk directly, so there is
// no envelope to build. It is a real backend rather than a special case in the
// image builder, because the moment one board skips the interface, the
// interface stops being the thing every board goes through.
type virt struct{}

func (virt) ID() string { return "virt" }

func (virt) Describe() string {
	return "no installer: QEMU is given the kernel, initramfs and disk directly"
}

func (virt) Wrap(img Image, outDir string, log io.Writer) (string, error) {
	if img.Disk == "" {
		return "", fmt.Errorf("virt needs a disk image")
	}
	fmt.Fprintf(log, "==> %s boots directly; nothing to wrap\n", img.Board)
	return filepath.Clean(img.Disk), nil
}
