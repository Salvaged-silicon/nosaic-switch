package pkgbuild

import (
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/nospkg"
	"github.com/salvaged-silicon/nosaic-switch/internal/svcgen"
)

// prefix is where packages install inside the image. Everything NOSaic builds
// lands under /usr; there is no /usr/local, because there is no distinction
// between "the distribution" and "locally installed" in an image composed
// entirely from packages.
const prefix = "/usr"

// buildEnv returns the environment a cross-build needs.
//
// Every variable is set explicitly rather than relying on the toolchain being
// first in PATH. A build that silently picks up the host compiler produces
// x86 objects for a PowerPC switch, and the failure appears at link time in
// another package entirely.
func buildEnv(o Options) []string {
	tc := filepath.Join(o.Root, "toolchain", o.Arch.ID)
	bin := filepath.Join(tc, "bin")
	t := o.Arch.Triple

	env := []string{
		"PATH=" + bin + ":" + os.Getenv("PATH"),
		"CHOST=" + t,
		"CBUILD=" + t,
		"CC=" + t + "-gcc",
		"CXX=" + t + "-g++",
		"AR=" + t + "-ar",
		"AS=" + t + "-as",
		"LD=" + t + "-ld",
		"NM=" + t + "-nm",
		"RANLIB=" + t + "-ranlib",
		"STRIP=" + t + "-strip",
		"OBJCOPY=" + t + "-objcopy",
		"OBJDUMP=" + t + "-objdump",
		"CFLAGS=-O2 -pipe",
		"CXXFLAGS=-O2 -pipe",
		// Reproducibility: anything embedding a build timestamp must use
		// this rather than the clock.
		"SOURCE_DATE_EPOCH=" + strconv.FormatInt(effectiveEpoch(o), 10),
		"LC_ALL=C",
		"TZ=UTC",
	}
	for k, v := range o.Recipe.Build.Env {
		env = append(env, k+"="+expand(o, v))
	}
	sort.Strings(env)
	return env
}

func effectiveEpoch(o Options) int64 {
	if o.Epoch != 0 {
		return o.Epoch
	}
	return defaultEpoch
}

// expand substitutes the few placeholders a recipe may use. Keeping the set
// tiny and explicit is deliberate: a recipe that can run arbitrary shell is a
// build script again, which is what this design exists to avoid.
func expand(o Options, s string) string {
	r := strings.NewReplacer(
		"${TRIPLE}", o.Arch.Triple,
		"${ARCH}", o.Arch.ID,
		"${PREFIX}", prefix,
		"${KERNEL_ARCH}", o.Arch.KernelArch,
	)
	return r.Replace(s)
}

func run(o Options, dir string, env []string, name string, args ...string) error {
	fmt.Fprintf(o.Log, "    $ %s %s\n", name, strings.Join(args, " "))
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	cmd.Env = env
	out, err := cmd.CombinedOutput()
	if err != nil {
		tail := string(out)
		if len(tail) > 4000 {
			tail = "...\n" + tail[len(tail)-4000:]
		}
		return fmt.Errorf("%s %s: %w\n%s", name, strings.Join(args, " "), err, tail)
	}
	return nil
}

// runBuild configures, compiles and stage-installs the package.
func runBuild(o Options, srcDir, stage string) error {
	b := o.Recipe.Build
	env := buildEnv(o)
	jobs := "-j" + strconv.Itoa(o.Jobs)

	switch b.System {
	case "configure", "autotools":
		args := []string{"--prefix=" + prefix}
		for _, a := range b.Configure {
			args = append(args, expand(o, a))
		}
		if err := run(o, srcDir, env, "./configure", args...); err != nil {
			return err
		}
		if err := run(o, srcDir, env, "make", jobs); err != nil {
			return err
		}
		return run(o, srcDir, env, "make", "DESTDIR="+stage, "install")

	case "make":
		args := append([]string{jobs}, b.Targets...)
		if err := run(o, srcDir, env, "make", args...); err != nil {
			return err
		}
		return run(o, srcDir, env, "make", "DESTDIR="+stage, "install")

	default:
		return fmt.Errorf("unknown build system %q (known: configure, autotools, make, none)", b.System)
	}
}

// collect turns the staging tree plus the recipe's explicit install entries
// into package entries.
func collect(o Options, stage string) ([]nospkg.Entry, error) {
	var entries []nospkg.Entry

	err := filepath.WalkDir(stage, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(stage, p)
		if err != nil {
			return err
		}
		if rel == "." {
			return nil
		}
		dst := "/" + filepath.ToSlash(rel)

		info, err := d.Info()
		if err != nil {
			return err
		}
		switch {
		case d.IsDir():
			entries = append(entries, nospkg.Entry{Dst: dst, Dir: true, Mode: uint32(info.Mode().Perm())})
		case info.Mode()&os.ModeSymlink != 0:
			target, err := os.Readlink(p)
			if err != nil {
				return err
			}
			entries = append(entries, nospkg.Entry{Dst: dst, Link: target})
		case info.Mode().IsRegular():
			entries = append(entries, nospkg.Entry{Dst: dst, Src: p, Mode: uint32(info.Mode().Perm())})
		default:
			// Sockets, devices and the like have no business in a package.
			return fmt.Errorf("%s: unsupported file type %v", dst, info.Mode().Type())
		}
		return nil
	})
	if err != nil && !os.IsNotExist(err) {
		return nil, err
	}

	// Explicit install entries, relative to the recipe's directory. These are
	// for files that are part of the recipe rather than of the source:
	// configuration, data, board files.
	recipeDir := filepath.Dir(o.Recipe.Path)
	for _, in := range o.Recipe.Install {
		mode := uint32(0o644)
		if in.Mode != "" {
			m, err := strconv.ParseUint(in.Mode, 8, 32)
			if err != nil {
				return nil, fmt.Errorf("install %s: mode %q is not octal", in.Dst, in.Mode)
			}
			mode = uint32(m)
		}
		src := in.Src
		if !filepath.IsAbs(src) {
			src = filepath.Join(recipeDir, in.Src)
		}
		if _, err := os.Stat(src); err != nil {
			return nil, fmt.Errorf("install %s: %w", in.Dst, err)
		}
		entries = append(entries, nospkg.Entry{Dst: in.Dst, Src: src, Mode: mode})
	}
	return entries, nil
}

// generateServices renders init files for every backend, so one package works
// on a systemd image and an s6 image alike.
func generateServices(o Options, work string) ([]nospkg.Entry, error) {
	if len(o.Recipe.Services) == 0 {
		return nil, nil
	}
	dir := filepath.Join(work, "services")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}

	var entries []nospkg.Entry
	n := 0
	for _, rs := range o.Recipe.Services {
		s := svcgen.Service{
			Name: rs.Name, Exec: expand(o, rs.Exec), After: rs.After,
			Wants: rs.Wants, Restart: rs.Restart,
		}
		for _, backend := range svcgen.All() {
			files, err := backend.Generate(s)
			if err != nil {
				return nil, fmt.Errorf("service %s (%s): %w", rs.Name, backend.ID(), err)
			}
			for _, f := range files {
				n++
				tmp := filepath.Join(dir, strconv.Itoa(n))
				if err := os.WriteFile(tmp, []byte(f.Content), 0o644); err != nil {
					return nil, err
				}
				entries = append(entries, nospkg.Entry{Dst: f.Path, Src: tmp, Mode: f.Mode})
			}
		}
	}
	return entries, nil
}
