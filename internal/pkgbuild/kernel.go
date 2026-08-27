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

	if len(b.Fragments) > 0 {
		var data []byte
		for _, name := range b.Fragments {
			frag := filepath.Join(filepath.Dir(o.Recipe.Path), expand(o, name))
			part, err := os.ReadFile(frag)
			if err != nil {
				return fmt.Errorf("kernel fragment: %w", err)
			}
			data = append(append(data, part...), '\n')
		}
		f, err := os.OpenFile(filepath.Join(srcDir, ".config"), os.O_APPEND|os.O_WRONLY, 0o644)
		if err != nil {
			return err
		}
		if _, err := f.Write(append([]byte("\n# --- NOSaic fragments ---\n"), data...)); err != nil {
			f.Close()
			return err
		}
		f.Close()

		// olddefconfig resolves the fragment against the defconfig and fills
		// in anything newly implied.
		if err := run(o, srcDir, env, "make", "olddefconfig"); err != nil {
			return err
		}

		// Appending to .config is only a request: a symbol whose dependencies
		// are unmet is silently dropped. Verifying afterwards is what turns a
		// silently missing filesystem into a build error rather than a kernel
		// that boots and then cannot mount its root.
		if err := verifyFragment(srcDir, data); err != nil {
			return err
		}
	}

	if err := run(o, srcDir, env, "make", jobs); err != nil {
		return err
	}

	modStage := stage
	if err := run(o, srcDir, env, "make",
		"INSTALL_MOD_PATH="+modStage, "INSTALL_MOD_STRIP=1", "modules_install"); err != nil {
		return err
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
		if line == "" || strings.HasPrefix(line, "#") {
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
