package pkgbuild

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// runKernelHeaders packages what an out-of-tree module needs to compile
// against the kernel this tree just built.
//
// It is a separate package from linux on purpose. Whatever the linux package
// contains ends up in the image, and a switch never builds a module on itself:
// shipping the build tree would roughly double the image to carry something
// nothing on the box uses. As its own package it is staged when a module
// recipe depends on it and is absent from every profile, so it is present at
// build time and nowhere else.
//
// It reads the tree the linux recipe built rather than unpacking the kernel
// again. The two must be the same tree -- a module built against different
// headers than the running kernel loads and then misbehaves, which is a far
// worse failure than not building.
func runKernelHeaders(o Options, stage string) error {
	ver := o.Recipe.Version
	// Arch-scoped, matching the work directory in pkgbuild.go.
	src := filepath.Join(o.Root, ".cache", "pkg", "linux", o.Arch.ID, "src", "linux-"+ver)
	if _, err := os.Stat(src); err != nil {
		return fmt.Errorf("no built kernel tree at %s: build the linux recipe first: %w",
			relPath(o.Root, src), err)
	}

	dst := filepath.Join(stage, "usr", "lib", "modules", ver, "build")
	if err := os.MkdirAll(dst, 0o755); err != nil {
		return err
	}

	// The subset kbuild needs for M= builds. scripts/ must be the built one,
	// not a pristine copy: modpost and the rest are compiled during the kernel
	// build and an external module build invokes them.
	want := []string{
		"Makefile",
		"Module.symvers",
		".config",
		"scripts",
		"include",
		filepath.Join("arch", o.Arch.KernelArch, "include"),
		filepath.Join("arch", o.Arch.KernelArch, "Makefile"),
	}
	// objtool is required whenever the kernel was configured with it, and the
	// failure without it is a link-time error in the module rather than
	// anything naming objtool.
	if _, err := os.Stat(filepath.Join(src, "tools", "objtool", "objtool")); err == nil {
		want = append(want, filepath.Join("tools", "objtool"))
	}

	n := 0
	for _, rel := range want {
		from := filepath.Join(src, rel)
		if _, err := os.Lstat(from); err != nil {
			if rel == "Module.symvers" {
				// Without it every symbol the module imports is unknown, and
				// the module loads with unresolved references at insmod time.
				return fmt.Errorf("the kernel tree has no Module.symvers; it was not fully built")
			}
			continue
		}
		to := filepath.Join(dst, rel)
		if err := os.MkdirAll(filepath.Dir(to), 0o755); err != nil {
			return err
		}
		c, err := copyTree(from, to)
		if err != nil {
			return fmt.Errorf("%s: %w", rel, err)
		}
		n += c
	}
	fmt.Fprintf(o.Log, "    %d files of kernel build tree for %s\n", n, ver)
	return nil
}

// copyTree copies a file or directory, preserving mode and symlinks, and
// returns how many files it wrote.
func copyTree(from, to string) (int, error) {
	fi, err := os.Lstat(from)
	if err != nil {
		return 0, err
	}
	switch {
	case fi.Mode()&os.ModeSymlink != 0:
		target, err := os.Readlink(from)
		if err != nil {
			return 0, err
		}
		_ = os.Remove(to)
		return 1, os.Symlink(target, to)
	case fi.IsDir():
		if err := os.MkdirAll(to, fi.Mode().Perm()); err != nil {
			return 0, err
		}
		entries, err := os.ReadDir(from)
		if err != nil {
			return 0, err
		}
		n := 0
		for _, e := range entries {
			c, err := copyTree(filepath.Join(from, e.Name()), filepath.Join(to, e.Name()))
			if err != nil {
				return n, err
			}
			n += c
		}
		return n, nil
	default:
		return 1, copyFile(from, to, fi.Mode().Perm())
	}
}

func relPath(root, p string) string {
	if r, err := filepath.Rel(root, p); err == nil && !strings.HasPrefix(r, "..") {
		return r
	}
	return p
}
