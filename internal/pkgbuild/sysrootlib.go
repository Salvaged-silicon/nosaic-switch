package pkgbuild

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

// runSysrootLibc packages the C library out of the toolchain's sysroot.
//
// glibc is not built by a recipe: crosstool-NG builds it as part of producing
// the toolchain, and building a second copy would risk the image's libc and
// the one everything was compiled against disagreeing -- which is a class of
// bug that appears as inexplicable crashes rather than as a link error.
//
// So this packages what the toolchain already produced. It exists because an
// image otherwise has no libc at all: every package built so far was either
// static or never executed on the box, and the gap only appeared when s6 --
// the first dynamically linked thing that actually runs -- could not start,
// with the shell reporting nothing at all.
func runSysrootLibc(o Options, stage string) error {
	sysroot := filepath.Join(o.Root, "toolchain", o.Arch.ID, o.Arch.Triple, "sysroot")
	if _, err := os.Stat(sysroot); err != nil {
		return fmt.Errorf("no sysroot for %s: build the toolchain first: %w", o.Arch.ID, err)
	}

	n := 0
	for _, dir := range []string{"lib", "lib64", "usr/lib", "usr/lib64"} {
		src := filepath.Join(sysroot, dir)
		fi, err := os.Lstat(src)
		if err != nil {
			continue
		}

		// A whole library directory is often a symlink -- on x86_64, lib64
		// points at lib. That symlink is load-bearing: binaries carry
		// /lib64/ld-linux-x86-64.so.2 as their interpreter, and without it the
		// kernel cannot start them at all. Not "cannot find a library": cannot
		// execute, with no process to report anything, which is why the first
		// symptom here was an init that died in complete silence.
		if fi.Mode()&os.ModeSymlink != 0 {
			target, err := os.Readlink(src)
			if err != nil {
				return err
			}
			dst := filepath.Join(stage, dir)
			if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
				return err
			}
			_ = os.Remove(dst)
			if err := os.Symlink(target, dst); err != nil {
				return err
			}
			n++
			continue
		}
		err = filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			rel, _ := filepath.Rel(sysroot, p)
			dst := filepath.Join(stage, rel)

			if d.IsDir() {
				return os.MkdirAll(dst, 0o755)
			}
			name := d.Name()
			// Runtime only. Static archives, libtool files and linker scripts
			// belong in a build sysroot, not in an image: they are several
			// times the size of what runs and nothing on a switch links.
			isShared := strings.Contains(name, ".so")
			if !isShared || strings.HasSuffix(name, ".a") || strings.HasSuffix(name, ".la") ||
				strings.HasSuffix(name, ".o") {
				return nil
			}
			info, err := d.Info()
			if err != nil {
				return err
			}
			if info.Mode()&os.ModeSymlink != 0 {
				target, err := os.Readlink(p)
				if err != nil {
					return err
				}
				_ = os.Remove(dst)
				n++
				return os.Symlink(target, dst)
			}
			b, err := os.ReadFile(p)
			if err != nil {
				return err
			}
			n++
			return os.WriteFile(dst, b, info.Mode().Perm())
		})
		if err != nil {
			return err
		}
	}
	if n == 0 {
		return fmt.Errorf("found no shared libraries in %s", sysroot)
	}
	fmt.Fprintf(o.Log, "    %d runtime libraries from the toolchain sysroot\n", n)
	return nil
}
