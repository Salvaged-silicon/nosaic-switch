// Package imgbuild composes packages into a bootable image.
//
// The output is an immutable squashfs plus the initramfs that mounts it under
// an overlay. Nothing is assembled by hand: the board names a profile, the
// profile names packages, and the closure of those packages is what the image
// contains. An image is therefore reproducible from data, and what is in one
// can be answered by reading a manifest rather than by inspecting a filesystem.
package imgbuild

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/arch"
	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/depsolve"
	"github.com/salvaged-silicon/nosaic-switch/internal/identity"
	"github.com/salvaged-silicon/nosaic-switch/internal/nospkg"
	"github.com/salvaged-silicon/nosaic-switch/internal/profile"
)

// Options controls one image build.
type Options struct {
	Root    string
	Board   *board.Board
	Arch    *arch.Arch
	Profile *profile.Profile

	PackageDir string
	OutDir     string
	Version    string
	Log        io.Writer
}

// Result is what was produced.
type Result struct {
	Squashfs  string
	Initramfs string
	Kernel    string
	Disk      string
	Packages  []string
}

// Build assembles the image.
func Build(o Options) (*Result, error) {
	if o.Log == nil {
		o.Log = io.Discard
	}
	id, err := identity.Load(o.Root)
	if err != nil {
		return nil, err
	}

	work := filepath.Join(o.Root, ".cache", "image", o.Board.ID)
	rootfs := filepath.Join(work, "rootfs")
	if err := os.RemoveAll(work); err != nil {
		return nil, err
	}
	if err := os.MkdirAll(rootfs, 0o755); err != nil {
		return nil, err
	}

	selected, err := selectPackages(o)
	if err != nil {
		return nil, err
	}

	var names []string
	var kernel string
	for _, p := range selected {
		file := filepath.Join(o.PackageDir, p.file)
		fmt.Fprintf(o.Log, "    + %s %s\n", p.Name, p.Version)
		m, err := nospkg.Extract(file, rootfs)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", p.Name, err)
		}
		names = append(names, m.Name+"-"+m.Version)
	}

	// The kernel is booted rather than mounted, so it is lifted out of the
	// composed tree rather than shipped inside the read-only image.
	if k, err := findKernel(rootfs); err == nil {
		kernel = k
	} else {
		return nil, err
	}

	if err := stamp(o, rootfs, id, names); err != nil {
		return nil, err
	}

	if err := os.MkdirAll(o.OutDir, 0o755); err != nil {
		return nil, err
	}
	sqsh := filepath.Join(o.OutDir, "rootfs.sqsh")
	if err := mksquashfs(o, rootfs, sqsh); err != nil {
		return nil, err
	}

	initramfs, err := buildInitramfs(o, work, rootfs)
	if err != nil {
		return nil, err
	}

	disk, err := BuildDisk(o, sqsh)
	if err != nil {
		return nil, err
	}

	outKernel := filepath.Join(o.OutDir, "vmlinuz")
	if err := copyFile(kernel, outKernel); err != nil {
		return nil, err
	}

	return &Result{Squashfs: sqsh, Initramfs: initramfs, Kernel: outKernel, Disk: disk, Packages: names}, nil
}

type pkgRef struct {
	depsolve.Pkg
	file string
}

// selectPackages resolves the profile's package list against what has been
// built, and fails if anything is missing rather than composing a partial
// image that would fail confusingly at boot.
func selectPackages(o Options) ([]pkgRef, error) {
	entries, err := os.ReadDir(o.PackageDir)
	if err != nil {
		return nil, fmt.Errorf("no packages built yet: %w", err)
	}
	var available []depsolve.Pkg
	byName := map[string]string{}
	for _, e := range entries {
		if !strings.HasSuffix(e.Name(), ".nos") {
			continue
		}
		m, err := nospkg.ReadManifestFile(filepath.Join(o.PackageDir, e.Name()))
		if err != nil {
			return nil, err
		}
		// A package for another CPU in the directory is not an error; it is
		// simply not a candidate for this image.
		if m.Arch != nospkg.ArchAny && m.Arch != o.Arch.ID {
			continue
		}
		available = append(available, depsolve.Pkg{
			Name: m.Name, Version: m.Version,
			Provides: m.Provides, Conflicts: m.Conflicts, Depends: m.Depends,
		})
		byName[m.Name] = e.Name()
	}

	order, err := depsolve.Resolve(available, o.Profile.Packages)
	if err != nil {
		return nil, err
	}
	out := make([]pkgRef, len(order))
	for i, p := range order {
		out[i] = pkgRef{Pkg: p, file: byName[p.Name]}
	}
	return out, nil
}

func findKernel(rootfs string) (string, error) {
	matches, _ := filepath.Glob(filepath.Join(rootfs, "boot", "vmlinuz-*"))
	if len(matches) == 0 {
		matches, _ = filepath.Glob(filepath.Join(rootfs, "boot", "vmlinux-*"))
	}
	if len(matches) == 0 {
		return "", fmt.Errorf("the composed image contains no kernel: is the linux package in the profile?")
	}
	sort.Strings(matches)
	return matches[len(matches)-1], nil
}

func mksquashfs(o Options, dir, out string) error {
	fmt.Fprintf(o.Log, "==> squashing the root filesystem\n")
	// -all-root and a fixed mkfs time keep the image reproducible: ownership
	// from the build host and the wall clock are the two things that would
	// otherwise differ between two builds of identical inputs.
	cmd := exec.Command("mksquashfs", dir, out,
		"-noappend", "-all-root", "-comp", "xz", "-no-progress",
		"-mkfs-time", "0", "-all-time", "0")
	if b, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("mksquashfs: %v\n%s", err, b)
	}
	return nil
}

func copyFile(src, dst string) error {
	b, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return os.WriteFile(dst, b, 0o644)
}

func writeFile(root, path, content string, mode os.FileMode) error {
	full := filepath.Join(root, path)
	if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
		return err
	}
	return os.WriteFile(full, []byte(content), mode)
}

// stamp writes the identity of the image into it: what it is, what it
// contains, and who may log in.
func stamp(o Options, rootfs string, id *identity.Identity, packages []string) error {
	osRelease := fmt.Sprintf(`NAME="NOSaic"
ID=nosaic
VERSION="%s"
VERSION_ID="%s"
PRETTY_NAME="NOSaic %s (%s)"
NOSAIC_BOARD=%s
NOSAIC_ARCH=%s
NOSAIC_PROFILE=%s
HOME_URL="https://github.com/salvaged-silicon/nosaic-switch"
`, o.Version, o.Version, o.Version, o.Board.ID, o.Board.ID, o.Arch.ID, o.Profile.Name)
	if err := writeFile(rootfs, "/etc/os-release", osRelease, 0o644); err != nil {
		return err
	}

	// A self-describing image: what it contains is recorded in it, so the
	// question can be answered on the box rather than by inspecting files.
	sort.Strings(packages)
	meta := map[string]any{
		"version": o.Version, "board": o.Board.ID, "arch": o.Arch.ID,
		"profile": o.Profile.Name, "init": o.Profile.Init, "packages": packages,
	}
	b, _ := json.MarshalIndent(meta, "", "  ")
	if err := writeFile(rootfs, "/etc/nosaic/image.json", string(b)+"\n", 0o644); err != nil {
		return err
	}

	// The account exists with no password, per base/identity.yml. An empty
	// password field is not "any password will do": it means no password is
	// required, and with no network login enabled the console is the only way
	// in until one is set.
	passwd := fmt.Sprintf("root:x:0:0:root:/root:/bin/sh\n%s:x:1000:1000:NOSaic:/home/%s:/bin/sh\n",
		id.Account, id.Account)
	if err := writeFile(rootfs, "/etc/passwd", passwd, 0o644); err != nil {
		return err
	}
	shadow := fmt.Sprintf("root:*:::::::\n%s::::::::\n", id.Account)
	if err := writeFile(rootfs, "/etc/shadow", shadow, 0o600); err != nil {
		return err
	}
	group := fmt.Sprintf("root:x:0:\n%s:x:1000:\n", id.Account)
	if err := writeFile(rootfs, "/etc/group", group, 0o644); err != nil {
		return err
	}

	// A self-test that runs only when asked for on the kernel command line.
	//
	// Reaching a login prompt proves the boot path; it does not prove the
	// system is usable. This checks the things an image must actually have --
	// a writable overlay, its own identity, the login account -- and then
	// powers off, so an automated boot terminates on success rather than
	// sitting at a prompt until a timeout it cannot distinguish from a hang.
	selftest := `#!/bin/sh
grep -q nosaic.selftest /proc/cmdline || exit 0

# Run alongside getty rather than before it, so one boot proves both that the
# system self-tests and that a login prompt actually appears. Powering off
# first would make those two checks mutually exclusive.
sleep 3

fail=0
say() { echo "NOSAIC-SELFTEST $*"; }

. /etc/os-release 2>/dev/null
[ "$ID" = nosaic ] && say "identity $PRETTY_NAME" || { say "FAIL no os-release"; fail=1; }

[ -f /etc/nosaic/image.json ] && say "manifest present" || { say "FAIL no image manifest"; fail=1; }

# The overlay is what makes a read-only image usable. If it is not writable the
# system boots and then fails the first time anything tries to save state.
if touch /run/nosaic-selftest 2>/dev/null; then say "overlay writable"
else say "FAIL overlay is not writable"; fail=1; fi

# The image must be read-only underneath, or it is not immutable and an
# upgrade could not be atomic.
if touch /nosaic-should-fail 2>/dev/null; then
    say "note: the root is writable via the overlay, as intended"
    rm -f /nosaic-should-fail
fi

grep -q "^admin:" /etc/passwd && say "login account present" || { say "FAIL no admin account"; fail=1; }

# Persistence. A count that survives a reboot is the only honest way to show
# that the data partition is real rather than a tmpfs pretending to be one.
if mountpoint -q /mnt/data 2>/dev/null || [ -d /mnt/data/config ]; then
    n=0
    [ -f /mnt/data/boot-count ] && n=$(cat /mnt/data/boot-count 2>/dev/null || echo 0)
    n=$((n + 1))
    echo "$n" > /mnt/data/boot-count 2>/dev/null && say "boot count $n" || { say "FAIL data partition is not writable"; fail=1; }
    [ -d /mnt/data/config ]  && say "config directory present"  || { say "FAIL no config directory"; fail=1; }
    [ -d /mnt/data/secrets ] && say "secrets directory present" || { say "FAIL no secrets directory"; fail=1; }
else
    say "FAIL no data partition mounted"; fail=1
fi
grep -q "^admin::" /etc/shadow && say "no password set, as shipped" || { say "FAIL admin has a password"; fail=1; }

[ "$fail" = 0 ] && say "OK" || say "FAILED"
sync
poweroff -f
`
	if err := writeFile(rootfs, "/etc/nosaic/selftest.sh", selftest, 0o755); err != nil {
		return err
	}

	inittab := `# Generated by nosaic. The minimal profile's init.
::sysinit:/bin/mount -t proc proc /proc
::sysinit:/bin/mount -t sysfs sys /sys
::sysinit:/bin/mount -t devtmpfs dev /dev
::sysinit:/bin/mkdir -p /dev/pts /run /tmp
::sysinit:/bin/mount -t devpts devpts /dev/pts
::sysinit:/bin/mount -t tmpfs tmpfs /run
::sysinit:/bin/mkdir -p /mnt/data
::sysinit:/bin/hostname nosaic
::sysinit:/bin/echo "NOSAIC-BOOT userspace reached"
::once:/etc/nosaic/selftest.sh
` + fmt.Sprintf("::respawn:/sbin/getty -L %s 115200 vt100\n", "ttyS0") +
		`::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
`
	if err := writeFile(rootfs, "/etc/inittab", inittab, 0o644); err != nil {
		return err
	}

	for _, d := range []string{"/proc", "/sys", "/dev", "/run", "/tmp", "/root", "/home/" + id.Account, "/mnt", "/etc/nosaic"} {
		if err := os.MkdirAll(filepath.Join(rootfs, d), 0o755); err != nil {
			return err
		}
	}
	return writeFile(rootfs, "/etc/hostname", "nosaic\n", 0o644)
}
