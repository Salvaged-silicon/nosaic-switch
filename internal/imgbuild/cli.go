package imgbuild

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
)

// installCLI builds the nosaic CLI for the target and puts it in the image.
//
// The CLI is not an optional extra on a switch: it is how an operator reads
// the platform, and the cooling loop is one of its subcommands, so an image
// without it boots with no thermal control and no way to ask why.
//
// Built here rather than packaged as a recipe because it is pure Go with cgo
// disabled -- no sysroot, no cross toolchain, nothing to link against. A
// recipe would mean copying the whole repository into a build tree to compile
// one static binary.
func installCLI(root, rootfs, goarch string, log io.Writer) error {
	out := filepath.Join(rootfs, "usr", "bin", "nosaic")
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		return err
	}

	cmd := exec.Command("go", "build",
		"-trimpath",
		// Stripped: this rides in every image and the debug information is
		// most of its size.
		"-ldflags", "-s -w",
		"-o", out, "./cmd/nosaic")
	cmd.Dir = root
	cmd.Env = append(os.Environ(),
		"CGO_ENABLED=0",
		"GOOS=linux",
		"GOARCH="+goarch,
	)
	if b, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("building the nosaic CLI for %s: %w\n%s", goarch, err, b)
	}

	fi, err := os.Stat(out)
	if err != nil {
		return err
	}
	fmt.Fprintf(log, "    nosaic CLI %.1f MiB into /usr/bin\n",
		float64(fi.Size())/(1<<20))
	return chmodRaw(out, 0o755)
}
