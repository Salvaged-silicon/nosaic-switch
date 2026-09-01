package pkgbuild

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// kernelEnv is deliberately not buildEnv.
//
// The kernel build sets its own compiler flags and derives its cross compiler
// from CROSS_COMPILE. Handing it the CC and CFLAGS a userspace package needs
// causes two distinct problems: CFLAGS meant for the target leak into the host
// tools the kernel builds for itself, and an explicit CC fights the kernel's
// own logic. So this passes ARCH and CROSS_COMPILE and stays out of the way.
func kernelEnv(o Options) []string {
	tc := filepath.Join(o.Root, "toolchain", o.Arch.ID)
	env := []string{
		"PATH=" + filepath.Join(tc, "bin") + ":" + os.Getenv("PATH"),
		"ARCH=" + o.Arch.KernelArch,
		"CROSS_COMPILE=" + o.Arch.Triple + "-",
		"LC_ALL=C",
		"TZ=UTC",

		// The kernel stamps the building user, host and time into the image
		// and into `uname`. Left alone, two builds of one source produce
		// different kernels, which would quietly undo the reproducibility the
		// package format works to provide.
		"KBUILD_BUILD_USER=nosaic",
		"KBUILD_BUILD_HOST=nosaic",
		"SOURCE_DATE_EPOCH=" + strconv.FormatInt(effectiveEpoch(o), 10),
	}
	for k, v := range o.Recipe.Build.Env {
		env = append(env, k+"="+expand(o, v))
	}
	return env
}

// runKernelBuild configures and builds a kernel, then stages the image, the
// modules and the config that produced them.
func runKernelBuild(o Options, srcDir, stage string) error {
	b := o.Recipe.Build
	defconfig := b.Defconfig
	if defconfig == "" {
		defconfig = o.Arch.KernelDefconfig
	}
	if defconfig == "" {
		return fmt.Errorf("neither build.defconfig nor arch/%s/arch.yml's kernel_defconfig is set", o.Arch.ID)
	}
	if o.Arch.KernelImage == "" {
		return fmt.Errorf("arch/%s/arch.yml has no kernel_image", o.Arch.ID)
	}
	env := kernelEnv(o)
	jobs := "-j" + strconv.Itoa(o.Jobs)

	// Start from the kernel's own defconfig rather than committing a complete
	// .config. A full config is thousands of lines that nobody reviews and
	// that silently rots across versions; a defconfig plus a small fragment
	// makes our actual decisions legible.
	if err := run(o, srcDir, env, "make", defconfig); err != nil {
		return err
	}

	if err := applyFragments(o, srcDir, env, b.Fragments); err != nil {
		return err
	}

	if err := run(o, srcDir, env, "make", jobs); err != nil {
		return err
	}

	modStage := stage
	if err := run(o, srcDir, env, "make",
		"INSTALL_MOD_PATH="+modStage, "INSTALL_MOD_STRIP=1", "modules_install"); err != nil {
		return err
	}

	// modules_install leaves build/ and source/ symlinks pointing at the
	// directory the kernel was compiled in. That directory is inside the
	// builder container, so what ships is an absolute path into a filesystem
	// the switch has never seen -- /lib/modules/<ver>/build resolving to
	// nothing. depmod does not need them and nothing on a switch builds a
	// module in place, so they are removed rather than repointed.
	//
	// Out-of-tree modules are built against the kernel recipe's output at
	// build time, not on the box.
	modDir := filepath.Join(modStage, "lib", "modules", o.Recipe.Version)
	for _, link := range []string{"build", "source"} {
		p := filepath.Join(modDir, link)
		if fi, err := os.Lstat(p); err == nil && fi.Mode()&os.ModeSymlink != 0 {
			if err := os.Remove(p); err != nil {
				return err
			}
		}
	}

	// Stage the bootable image and the config that produced it. Shipping the
	// config matters: reproducing or debugging a kernel a year later starts
	// with knowing exactly how it was configured.
	boot := filepath.Join(stage, "boot")
	if err := os.MkdirAll(boot, 0o755); err != nil {
		return err
	}
	ver := o.Recipe.Version
	if err := copyFile(filepath.Join(srcDir, o.Arch.KernelImage),
		filepath.Join(boot, "vmlinuz-"+ver), 0o644); err != nil {
		return err
	}
	if err := copyFile(filepath.Join(srcDir, ".config"),
		filepath.Join(boot, "config-"+ver), 0o644); err != nil {
		return err
	}
	if err := copyFile(filepath.Join(srcDir, "System.map"),
		filepath.Join(boot, "System.map-"+ver), 0o644); err != nil {
		return err
	}
	return nil
}

// verifyFragment checks that every symbol we asked for actually took effect.
func verifyFragment(srcDir string, fragment []byte) error {
	got, err := os.ReadFile(filepath.Join(srcDir, ".config"))
	if err != nil {
		return err
	}
	have := map[string]string{}
	for _, line := range strings.Split(string(got), "\n") {
		if k, v, ok := strings.Cut(strings.TrimSpace(line), "="); ok && strings.HasPrefix(k, "CONFIG_") {
			have[k] = v
		}
	}

	var missing []string
	for _, line := range strings.Split(string(fragment), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		// "# CONFIG_X is not set" is a request, not a comment, and it is the
		// one most worth checking: a symbol a fragment turns off can be turned
		// back on by another symbol that selects it, and the result is a
		// kernel that differs from the one asked for in the direction nobody
		// looks. Everything else beginning with # really is a comment.
		if rest, ok := strings.CutPrefix(line, "# "); ok {
			sym, ok := strings.CutSuffix(rest, " is not set")
			if !ok || !strings.HasPrefix(sym, "CONFIG_") {
				continue
			}
			if v, on := have[sym]; on && v != "n" {
				missing = append(missing, fmt.Sprintf("%s: asked for it to be off, got %q", sym, v))
			}
			continue
		}
		if strings.HasPrefix(line, "#") {
			continue
		}
		k, want, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		// Built-in satisfies a request for a module: if a feature was wanted
		// loadable and is instead compiled in, the feature is present. The
		// reverse is not true, which is why boot-critical symbols are asked
		// for as y — a module cannot be loaded before the root it lives on
		// has been mounted.
		got := have[k]
		if got == want || (want == "m" && got == "y") {
			continue
		}
		missing = append(missing, fmt.Sprintf("%s: asked for %s, got %q", k, want, got))
	}
	if len(missing) > 0 {
		return fmt.Errorf("kernel configuration did not take effect:\n  %s\n"+
			"A symbol whose dependencies are unmet is dropped silently, which produces a "+
			"kernel that boots and then cannot do the thing you configured.",
			strings.Join(missing, "\n  "))
	}
	return nil
}

func copyFile(src, dst string, mode os.FileMode) error {
	b, err := os.ReadFile(src)
	if err != nil {
		return fmt.Errorf("staging %s: %w", filepath.Base(src), err)
	}
	return os.WriteFile(dst, b, mode)
}

// mergeConfig applies a fragment to a .config by replacing, not appending.
//
// Appending is not enough, and the difference is silent. Linux's kconfig lets
// a later assignment win, so appending happens to work there. busybox's older
// kconfig takes the *first* occurrence, so an appended CONFIG_STATIC=y loses
// to the "# CONFIG_STATIC is not set" that defconfig wrote a thousand lines
// earlier — and the build then quietly produces a dynamically linked binary
// that cannot run in an initramfs.
//
// So every symbol the fragment mentions is removed from the file first. This
// is what Linux's own merge_config.sh does, and it is correct for both.
func mergeConfig(path string, fragment []byte) error {
	set := map[string]bool{}
	for _, line := range strings.Split(string(fragment), "\n") {
		if sym, ok := symbolOf(strings.TrimSpace(line)); ok {
			set[sym] = true
		}
	}

	existing, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	var kept []string
	for _, line := range strings.Split(string(existing), "\n") {
		if sym, ok := symbolOf(strings.TrimSpace(line)); ok && set[sym] {
			continue // ours replaces it
		}
		kept = append(kept, line)
	}

	out := strings.Join(kept, "\n") + "\n# --- NOSaic fragments ---\n" + string(fragment)
	return os.WriteFile(path, []byte(out), 0o644)
}

// symbolOf recognises both forms kconfig writes: an assignment, and the
// "is not set" comment that means disabled.
func symbolOf(line string) (string, bool) {
	if rest, ok := strings.CutPrefix(line, "# "); ok {
		if sym, ok := strings.CutSuffix(rest, " is not set"); ok && strings.HasPrefix(sym, "CONFIG_") {
			return sym, true
		}
		return "", false
	}
	if sym, _, ok := strings.Cut(line, "="); ok && strings.HasPrefix(sym, "CONFIG_") {
		return sym, true
	}
	return "", false
}

// applyKconfig runs a defconfig and fragments for a package that configures
// the way the kernel does, which busybox and a few others do.
func applyKconfig(o Options, srcDir string, env []string) error {
	b := o.Recipe.Build
	if b.Defconfig == "" {
		return nil
	}
	if err := run(o, srcDir, env, "make", b.Defconfig); err != nil {
		return err
	}
	return applyFragments(o, srcDir, env, b.Fragments)
}

// applyFragments appends fragments to .config, resolves them, and then checks
// that every symbol asked for actually took effect.
func applyFragments(o Options, srcDir string, env []string, fragments []string) error {
	if len(fragments) == 0 {
		return nil
	}
	var data []byte
	for _, name := range fragments {
		frag := filepath.Join(filepath.Dir(o.Recipe.Path), expand(o, name))
		part, err := os.ReadFile(frag)
		if err != nil {
			return fmt.Errorf("config fragment: %w", err)
		}
		data = append(append(data, part...), '\n')
	}
	if err := mergeConfig(filepath.Join(srcDir, ".config"), data); err != nil {
		return err
	}

	target := o.Recipe.Build.ConfigTarget
	if target == "" {
		target = "olddefconfig"
	}
	if err := runInteractive(o, srcDir, env, "make", target); err != nil {
		return err
	}
	return verifyFragment(srcDir, data)
}
