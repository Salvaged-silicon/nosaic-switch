package pkgbuild

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/depsolve"
	"github.com/salvaged-silicon/nosaic-switch/internal/nospkg"
)

// stageDependencies materialises a package's dependencies so it can build
// against them.
//
// Until this existed, a recipe's depends: list was only used to order an
// install: nothing put the headers and libraries anywhere a compiler could
// find them, so a package needing another package's headers failed with a
// message about a missing header rather than a missing dependency. systemd
// found it, complaining that POSIX capability headers were absent when libcap
// had been built perfectly well an hour earlier.
//
// The staging sysroot is per-architecture and additive: each dependency is
// unpacked into it, so a package sees exactly what it declared and whatever
// those declared in turn. It is separate from the toolchain's own sysroot,
// which stays pristine -- a toolchain that accumulates project libraries is no
// longer reproducible from its config.
func stageDependencies(o Options) (string, error) {
	sysroot := filepath.Join(o.Root, ".cache", "sysroot", o.Arch.ID)
	// Everything this package needs to compile: what it links against at run
	// time, plus what it only needs while building.
	need := append(append([]string(nil), o.Recipe.Depends...), o.Recipe.BuildDepends...)
	if len(need) == 0 {
		return sysroot, os.MkdirAll(sysroot, 0o755)
	}

	entries, err := os.ReadDir(o.OutDir)
	if err != nil {
		return "", fmt.Errorf("%s depends on other packages, but none have been built: %w",
			o.Recipe.Name, err)
	}
	var available []depsolve.Pkg
	byName := map[string]string{}
	for _, e := range entries {
		if !strings.HasSuffix(e.Name(), ".nos") {
			continue
		}
		m, err := nospkg.ReadManifestFile(filepath.Join(o.OutDir, e.Name()))
		if err != nil {
			return "", err
		}
		if m.Arch != nospkg.ArchAny && m.Arch != o.Arch.ID {
			continue
		}
		available = append(available, depsolve.Pkg{
			Name: m.Name, Version: m.Version,
			Provides: m.Provides, Conflicts: m.Conflicts, Depends: m.Depends,
		})
		byName[m.Name] = e.Name()
	}

	// The transitive closure, in install order. Resolving rather than taking
	// the literal list means a dependency's own dependencies are staged too,
	// which is the difference between this working and working for one level.
	order, err := depsolve.Resolve(available, need)
	if err != nil {
		return "", fmt.Errorf("%s: %w — build its dependencies first", o.Recipe.Name, err)
	}

	if err := os.MkdirAll(sysroot, 0o755); err != nil {
		return "", err
	}
	for _, p := range order {
		file := filepath.Join(o.OutDir, byName[p.Name])
		fmt.Fprintf(o.Log, "    staging %s %s\n", p.Name, p.Version)
		if _, err := nospkg.Extract(file, sysroot); err != nil {
			return "", fmt.Errorf("staging %s: %w", p.Name, err)
		}
	}
	return sysroot, nil
}
