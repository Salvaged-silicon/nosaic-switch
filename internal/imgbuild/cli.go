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
func installCLI(root, rootfs, goarch string, log io.Writer) (bool, error) {
	// An architecture Go cannot target gets no CLI, and says so.
	//
	// GOARCH="" does not mean "pick something sensible", it means "build for
	// the host". Every PowerPC image built here therefore carried an x86-64
	// nosaic binary in /usr/bin, and nothing noticed, because the services
	// that run it are only emitted for boards with a platform HAL and this
	// board had none. The first thing to ask it anything got
	//
	//     /usr/bin/nosaic: line 11: syntax error: unexpected ")"
	//
	// which is a shell being handed an ELF for the wrong machine.
	//
	// The gc toolchain has ppc64 and ppc64le and has never had 32-bit
	// big-endian PowerPC, so arch/powerpc declares no go_arch and this is not
	// an omission to fix by guessing one. A board on such an architecture
	// ships without the CLI, and the caller must not generate services that
	// invoke it.
	if goarch == "" {
		fmt.Fprintf(log, "    no nosaic CLI: this architecture has no Go "+
			"target, so anything that needs it is left out\n")
		return false, nil
	}

	out := filepath.Join(rootfs, "usr", "bin", "nosaic")
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		return false, err
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
		return false, fmt.Errorf("building the nosaic CLI for %s: %w\n%s", goarch, err, b)
	}

	fi, err := os.Stat(out)
	if err != nil {
		return false, err
	}
	fmt.Fprintf(log, "    nosaic CLI %.1f MiB into /usr/bin\n",
		float64(fi.Size())/(1<<20))
	return true, chmodRaw(out, 0o755)
}
