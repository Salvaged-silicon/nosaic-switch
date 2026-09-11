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
// versionPkg is where the build identity lives. Stated once: a -X flag naming
// a package that has been moved fails silently -- the build succeeds and the
// variable keeps its default -- which is how the stamp went missing unnoticed.
const versionPkg = "github.com/salvaged-silicon/nosaic-switch/internal/version"

func installCLI(root, rootfs, goarch, ver, commit string, log io.Writer) (bool, error) {
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

	// Stripped, and stamped with the same version the image declares.
	//
	// The stamp was missing for as long as this function has existed, and the
	// result was a switch that disagreed with itself: `/etc/nosaic/image.json`
	// said 0.1.0 while `nosaic version` on the same box said 0.0.0-dev. The
	// Makefile's LDFLAGS reach `go build`, `go run` and `pkg build` -- every
	// build-host path -- and missed the one binary that actually ships,
	// because the CLI is not a package. It is pure static Go built in-tree,
	// so it sits outside the packaging that was taught to stamp itself.
	//
	// It reads as cosmetic and is not: the first question after an A/B upgrade
	// is which image you are on, and an unstamped CLI answers 0.0.0-dev from
	// both slots.
	cmd := exec.Command("go", "build",
		"-trimpath",
		"-ldflags", cliLDFlags(ver, commit),
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

// cliLDFlags is the link-time configuration for the shipped CLI: stripped, and
// stamped with the identity the image declares.
//
// An empty value is left at the package default rather than stamped as empty,
// so an offline build without a version produces a CLI that says 0.0.0-dev --
// which is true -- rather than one that says nothing at all.
func cliLDFlags(ver, commit string) string {
	f := "-s -w"
	if ver != "" {
		f += " -X " + versionPkg + ".Version=" + ver
	}
	if commit != "" {
		f += " -X " + versionPkg + ".Commit=" + commit
	}
	return f
}
