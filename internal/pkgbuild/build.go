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
	"syscall"

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

	sysroot := filepath.Join(tc, t, "sysroot")
	// Where this package's dependencies were staged. Searched before the
	// toolchain's own sysroot so a project-built library wins over anything
	// the toolchain happens to carry.
	staging := filepath.Join(o.Root, ".cache", "sysroot", o.Arch.ID)

	env := []string{
		"PATH=" + bin + ":" + os.Getenv("PATH"),

		// pkg-config must not see the build host's libraries.
		//
		// Left alone it reads /usr/lib/pkgconfig and reports that the host has
		// libelf, zlib, whatever -- and a configure script then enables a
		// feature whose headers do not exist in the target sysroot. The build
		// fails later, in a compile error that names a missing header rather
		// than the contamination that caused it. iproute2 found exactly this.
		//
		// LIBDIR replaces the search path entirely rather than adding to it,
		// which is the only version of this that actually works: appending
		// still leaves the host visible.
		"PKG_CONFIG_LIBDIR=" + filepath.Join(staging, "usr", "lib", "pkgconfig") +
			":" + filepath.Join(staging, "usr", "share", "pkgconfig") +
			":" + filepath.Join(sysroot, "usr", "lib", "pkgconfig") +
			":" + filepath.Join(sysroot, "usr", "share", "pkgconfig"),
		// Points at staging, not at the toolchain sysroot. pkg-config prefixes
		// every path from a .pc file with this, and the .pc files come from
		// packages we staged -- so pointing it at the toolchain root rewrote
		// util-linux's include path to somewhere that does not exist, and
		// systemd failed on a missing libmount.h while libmount was present.
		"PKG_CONFIG_SYSROOT_DIR=" + staging,
		"PKG_CONFIG_PATH=",
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
		"CFLAGS=-O2 -pipe -I" + filepath.Join(staging, "usr", "include"),
		"CXXFLAGS=-O2 -pipe -I" + filepath.Join(staging, "usr", "include"),
		// -L finds a library named on the command line. It does NOT find that
		// library's own dependencies: for a DT_NEEDED recorded inside a shared
		// object, GNU ld searches -rpath-link, then -rpath, then the defaults,
		// and never the -L paths. Without -rpath-link, linking an executable
		// against a staged .so that itself needs another staged .so fails --
		// and the error blames the intermediate library rather than the
		// missing search path:
		//
		//	warning: libmount.so.1, needed by libsystemd-core-257.so, not found
		//	libsystemd-shared-257.so: undefined reference to `mnt_free_iter'
		//
		// which reads as a broken library rather than a linker that was never
		// told where to look. It cost a long time on systemd.
		"LDFLAGS=-L" + filepath.Join(staging, "usr", "lib") +
			" -Wl,-rpath-link=" + filepath.Join(staging, "usr", "lib") +
			" -Wl,-rpath-link=" + filepath.Join(staging, "lib"),
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
		// Where this package's dependencies were staged. Most build systems
		// find them through pkg-config or the compiler's search path; a few
		// want to be told explicitly, and this is how a recipe says it.
		"${STAGING}", filepath.Join(o.Root, ".cache", "sysroot", o.Arch.ID),
	)
	return r.Replace(s)
}

// newlines answers an interactive prompt by accepting its default, forever.
//
// busybox's oldconfig asks about every symbol it cannot resolve and fails on a
// closed stdin. Feeding it defaults is the conventional answer; the fragment
// verification afterwards is what stops that being a blind "yes to everything",
// since anything we actually asked for is checked.
type newlines struct{}

func (newlines) Read(p []byte) (int, error) {
	for i := range p {
		p[i] = '\n'
	}
	return len(p), nil
}

func runInteractive(o Options, dir string, env []string, name string, args ...string) error {
	fmt.Fprintf(o.Log, "    $ %s %s\n", name, strings.Join(args, " "))
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	cmd.Env = env
	cmd.Stdin = newlines{}
	out, err := cmd.CombinedOutput()
	if err != nil {
		tail := string(out)
		if len(tail) > 2000 {
			tail = "...\n" + tail[len(tail)-2000:]
		}
		return fmt.Errorf("%s %s: %w\n%s", name, strings.Join(args, " "), err, tail)
	}
	return nil
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
	case "kernel":
		return runKernelBuild(o, srcDir, stage)

	case "meson":
		return runMesonBuild(o, srcDir, stage)

	case "kernel-headers":
		return runKernelHeaders(o, stage)

	case "sysroot-libc":
		return runSysrootLibc(o, stage)

	case "configure", "autotools":
		var args []string
		if !b.NoPrefix {
			args = append(args, "--prefix="+prefix)
		}
		// This is the difference between the two systems. A real autotools
		// configure needs --host to know it is cross-compiling; without it,
		// it probes the build machine, decides it can run test programs, and
		// produces a package built for the wrong CPU or one that fails
		// obscurely at link time. A hand-written configure (zlib's, for
		// instance) does not accept --host at all, which is why "configure"
		// exists as a separate system rather than being assumed.
		if b.System == "autotools" {
			args = append(args, "--host="+o.Arch.Triple)
		}
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
		// Directories the build writes into but does not create.
		for _, d := range b.Mkdirs {
			if err := os.MkdirAll(filepath.Join(srcDir, expand(o, d)), 0o755); err != nil {
				return err
			}
		}

		// A build that runs somewhere other than the source root.
		if b.Subdir != "" {
			srcDir = filepath.Join(srcDir, b.Subdir)
			if _, err := os.Stat(srcDir); err != nil {
				return fmt.Errorf("build.subdir %q is not in the source tree: %w", b.Subdir, err)
			}
		}

		// The compiler is passed as a make variable, not through the
		// environment, because these packages define it themselves. busybox's
		// Makefile says
		//
		//	CC = $(CROSS_COMPILE)gcc
		//
		// and an unconditional assignment in a Makefile beats the environment,
		// so CC=<triple>-gcc in env is simply ignored. A command-line variable
		// beats both.
		//
		// Without this the build silently uses the host's compiler. On x86_64
		// that produces a working package and nothing looks wrong, which is
		// exactly how it went unnoticed: it took the first aarch64 cross-build
		// to surface it, as an x86_64 busybox in a package targeting arm64.
		cross := "CROSS_COMPILE=" + o.Arch.Triple + "-"

		// Some packages configure through kconfig exactly as the kernel does.
		// Sharing that path means their configuration is a reviewable fragment
		// too, rather than a committed generated file.
		if err := applyKconfig(o, srcDir, env); err != nil {
			return err
		}
		args := append([]string{jobs, cross}, b.Targets...)
		if err := run(o, srcDir, env, "make", args...); err != nil {
			return err
		}
		// A build that stages by copying has no install target to run.
		if len(b.Stage) > 0 {
			return stagePaths(o, srcDir, stage)
		}

		install := []string{cross, "DESTDIR=" + stage, "install"}
		if len(b.InstallArgs) > 0 {
			install = []string{cross}
			for _, a := range b.InstallArgs {
				install = append(install, strings.ReplaceAll(expand(o, a), "${DESTDIR}", stage))
			}
		}
		return run(o, srcDir, env, "make", install...)

	default:
		return fmt.Errorf("unknown build system %q (known: configure, autotools, make, meson, kernel, kernel-headers, sysroot-libc, none)", b.System)
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
			entries = append(entries, nospkg.Entry{Dst: dst, Dir: true, Mode: rawMode(info)})
		case info.Mode()&os.ModeSymlink != 0:
			target, err := os.Readlink(p)
			if err != nil {
				return err
			}
			entries = append(entries, nospkg.Entry{Dst: dst, Link: target})
		case info.Mode().IsRegular():
			entries = append(entries, nospkg.Entry{Dst: dst, Src: p, Mode: rawMode(info)})
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

// stagePaths copies the recipe's stage: entries out of the build tree.
//
// srcDir here is the directory the build ran in, which may be a subdir; the
// paths are resolved against the source root instead, because what a build
// produces and where it is invoked from are different questions.
func stagePaths(o Options, srcDir, stage string) error {
	root := srcDir
	if o.Recipe.Build.Subdir != "" {
		root = strings.TrimSuffix(srcDir, string(os.PathSeparator)+o.Recipe.Build.Subdir)
	}
	n := 0
	for _, sp := range o.Recipe.Build.Stage {
		from := filepath.Join(root, expand(o, sp.Src))
		if _, err := os.Lstat(from); err != nil {
			return fmt.Errorf("stage %s: %w", sp.Src, err)
		}
		to := filepath.Join(stage, filepath.Clean("/"+expand(o, sp.Dst)))
		if err := os.MkdirAll(filepath.Dir(to), 0o755); err != nil {
			return err
		}
		c, err := copyTree(from, to)
		if err != nil {
			return fmt.Errorf("stage %s: %w", sp.Src, err)
		}
		if sp.Mode != "" {
			m, err := strconv.ParseUint(sp.Mode, 8, 32)
			if err != nil {
				return fmt.Errorf("stage %s: mode %q is not octal", sp.Src, sp.Mode)
			}
			if err := syscall.Chmod(to, uint32(m)); err != nil {
				return fmt.Errorf("stage %s: setting mode %s: %w", sp.Src, sp.Mode, err)
			}
		}
		n += c
	}
	fmt.Fprintf(o.Log, "    staged %d files\n", n)
	return nil
}

// rawMode returns a file's permission bits including set-id and sticky.
//
// FileMode.Perm() masks those off, so a package built from a staged tree lost
// the setuid bit on anything that had one -- silently, and only visible on the
// switch as a privilege helper that runs and does nothing.
func rawMode(info fs.FileInfo) uint32 {
	if st, ok := info.Sys().(*syscall.Stat_t); ok {
		return st.Mode & 0o7777
	}
	return uint32(info.Mode().Perm())
}
