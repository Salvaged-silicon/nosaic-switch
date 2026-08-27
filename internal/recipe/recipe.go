// Package recipe parses and validates NOSaic package recipes.
//
// A recipe is the declarative description of one package: where its source
// comes from, how to build it, what it installs, and what services it defines.
// It is the PKGBUILD analog. See docs/DESIGN.md.
//
// The schema here is deliberately minimal at M0 — it grows with the recipe
// engine at M2. What it already enforces are the invariants the design depends
// on, because a rule added after the tree fills up has already been bypassed.
package recipe

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"gopkg.in/yaml.v3"

	"github.com/salvaged-silicon/nosaic-switch/internal/svcgen"
)

// initOwnedDirs are paths an init system owns. A recipe installing into one is
// hand-writing a unit file, which breaks the minimal profile: that tier runs s6
// rather than systemd, so a hand-written systemd unit silently does nothing
// there. Declaring the service instead generates a definition for every init.
var initOwnedDirs = []string{
	"/etc/systemd/",
	"/usr/lib/systemd/",
	"/lib/systemd/",
	"/etc/s6-rc/",
	"/etc/s6/",
	"/etc/init.d/",
	"/etc/rc.d/",
}

// Source locates and pins upstream source. Vendor sources are pinned by hash
// and fetched at build time; they are never committed to the repository.
type Source struct {
	URL    string `yaml:"url"`
	SHA256 string `yaml:"sha256"`

	// Mirrors are tried in order if URL fails.
	//
	// This matters more here than in most projects. NOSaic exists for hardware
	// whose vendors have walked away, and the same happens to source archives:
	// a project's own download server is the first thing to disappear when it
	// is abandoned. Because every source is pinned by hash, a mirror cannot
	// substitute different content — so falling back costs nothing in trust.
	Mirrors []string `yaml:"mirrors"`
}

// Install maps one built file into the image.
type Install struct {
	Src  string `yaml:"src"`
	Dst  string `yaml:"dst"`
	Mode string `yaml:"mode"`
}

// Service is an init-system-agnostic service definition. Recipes never write
// unit files: systemd units and s6/OpenRC definitions are both generated from
// this, so the minimal profile can ship a different init without forking every
// recipe that starts a daemon.
type Service struct {
	Name    string   `yaml:"name"`
	Exec    string   `yaml:"exec"`
	After   []string `yaml:"after"`
	Wants   []string `yaml:"wants"`
	Restart string   `yaml:"restart"`
}

// Build describes how the package is compiled.
type Build struct {
	// System is how the package is built: configure, autotools, make, or none.
	System string `yaml:"system"`

	// Configure are extra arguments; --prefix is supplied automatically.
	// ${TRIPLE}, ${ARCH}, ${PREFIX} and ${KERNEL_ARCH} are substituted.
	Configure []string `yaml:"configure"`

	// Targets are make targets for system: make.
	Targets []string `yaml:"targets"`

	// Defconfig and Fragment are for system: kernel. The defconfig comes from
	// the kernel tree; the fragment is our own additions, kept small so the
	// decisions are legible rather than buried in thousands of generated lines.
	// Defconfig overrides the architecture's own kernel_defconfig.
	Defconfig string `yaml:"defconfig"`

	// Fragments are applied in order after the defconfig. ${ARCH} is
	// substituted, so one recipe covers every architecture.
	Fragments []string `yaml:"fragments"`

	// NoPrefix stops --prefix being passed to configure. Several projects
	// ship a hand-written configure that does not accept it and fails; that is
	// a property of the package, so it is recorded with the package.
	NoPrefix bool `yaml:"no_prefix"`

	// ConfigTarget resolves a .config after fragments are appended. The kernel
	// calls this olddefconfig; busybox's older kconfig calls it oldconfig.
	ConfigTarget string `yaml:"config_target"`

	// InstallArgs replaces the default "DESTDIR=<stage> install" for packages
	// whose build system spells staging differently. ${DESTDIR} is
	// substituted.
	InstallArgs []string `yaml:"install_args"`

	// Env overrides build environment variables. The cross-compilation
	// variables are set for you; this is for a package's own quirks.
	Env map[string]string `yaml:"env"`
}

// Recipe is one package.
type Recipe struct {
	Name    string `yaml:"name"`
	Version string `yaml:"version"`
	Summary string `yaml:"summary"`

	// License is an SPDX identifier, or a named vendor license for
	// source-available SDKs.
	License string `yaml:"license"`

	// Redistributable records whether this component may ship in a published
	// image. A pointer so that "unset" is distinguishable from "false" — an
	// omitted licensing decision must fail the check, not default to
	// permissive.
	Redistributable *bool `yaml:"redistributable"`

	// RuntimeInstallable allows this package to be installed onto a running
	// switch, not merely composed into an image. Most packages are not: a
	// libc cannot be swapped underneath a running system.
	RuntimeInstallable bool `yaml:"runtime_installable"`

	Source  *Source  `yaml:"source"`
	Patches []string `yaml:"patches"`

	Depends   []string `yaml:"depends"`
	Provides  []string `yaml:"provides"`
	Conflicts []string `yaml:"conflicts"`

	Build    *Build    `yaml:"build"`
	Install  []Install `yaml:"install"`
	Services []Service `yaml:"services"`

	// Path is where this recipe was loaded from. Not part of the file.
	Path string `yaml:"-"`
}

// Load reads and parses a single recipe.yml.
func Load(path string) (*Recipe, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var r Recipe
	dec := yaml.NewDecoder(strings.NewReader(string(b)))
	dec.KnownFields(true) // an unknown key is a typo, not an extension
	if err := dec.Decode(&r); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	r.Path = path
	return &r, nil
}

// LoadAll reads every recipes/<name>/recipe.yml under root.
func LoadAll(root string) ([]*Recipe, error) {
	dir := filepath.Join(root, "recipes")
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []*Recipe
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		p := filepath.Join(dir, e.Name(), "recipe.yml")
		if _, err := os.Stat(p); os.IsNotExist(err) {
			continue
		}
		r, err := Load(p)
		if err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

// Validate returns every problem with this recipe, not just the first — a
// contributor should see the whole list in one run.
func (r *Recipe) Validate() []string {
	var errs []string
	bad := func(f string, a ...any) { errs = append(errs, fmt.Sprintf(f, a...)) }

	if r.Name == "" {
		bad("name is required")
	}
	if r.Version == "" {
		bad("version is required")
	}

	// The licensing gate. NOSaic is Apache-2.0 but built images may carry
	// source-available vendor code, so every component must state what it is
	// and whether it may be published. Both are refused by omission rather
	// than assumed.
	if r.License == "" {
		bad("license is required (SPDX id, or the vendor license name)")
	}
	if r.Redistributable == nil {
		bad("redistributable must be set explicitly (true or false)")
	}

	if r.Source != nil {
		if r.Source.URL == "" {
			bad("source.url is required when source is present")
		}
		if r.Source.SHA256 == "" {
			bad("source.sha256 is required — sources are pinned by hash")
		} else if len(r.Source.SHA256) != 64 {
			bad("source.sha256 must be 64 hex characters, got %d", len(r.Source.SHA256))
		}
	}

	// Exclusive virtual names. A package providing a virtual name must also
	// conflict with it, or two implementations can co-install and fight over
	// the same hardware.
	for _, p := range r.Provides {
		if !contains(r.Conflicts, p) {
			bad("provides %q without conflicting with it: a virtual name must be exclusive, add %q to conflicts", p, p)
		}
	}

	// Validate through the generator, so a recipe cannot declare something
	// that only one init system could express.
	for i, s := range r.Services {
		gs := svcgen.Service{
			Name: s.Name, Exec: s.Exec, After: s.After,
			Wants: s.Wants, Restart: s.Restart,
		}
		if err := gs.Validate(); err != nil {
			bad("services[%d]: %v", i, err)
		}
	}

	for i, in := range r.Install {
		if in.Src == "" || in.Dst == "" {
			bad("install[%d]: both src and dst are required", i)
		}
		if in.Dst != "" && !strings.HasPrefix(in.Dst, "/") {
			bad("install[%d]: dst %q must be an absolute path in the image", i, in.Dst)
		}
		for _, d := range initOwnedDirs {
			if strings.HasPrefix(in.Dst, d) {
				bad("install[%d]: %q is owned by an init system — declare a services: entry instead, "+
					"so systemd and s6 definitions are both generated. A hand-written unit does "+
					"nothing on the minimal profile.", i, in.Dst)
			}
		}
	}

	return errs
}

func contains(hay []string, needle string) bool {
	for _, h := range hay {
		if h == needle {
			return true
		}
	}
	return false
}
