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

	// Local is a directory in this repository, relative to its root, built in
	// place of a fetched archive.
	//
	// NOSaic's own components are packaged by the same machinery as everything
	// else -- the datapath daemons especially, since a board's image resolves
	// `nosd` to one of them exactly as it resolves any other dependency.
	// Making them a special case outside the package system would mean the
	// component most likely to need a careful upgrade path is the one with
	// none.
	//
	// A local source is not hashed. The hash exists to prove a download was
	// not tampered with in transit; a directory in the tree is already covered
	// by the repository's own history.
	Local string `yaml:"local"`
}

// Install maps one built file into the image.
// StagePath copies a built path into the staging root.
type StagePath struct {
	Src string `yaml:"src"`
	Dst string `yaml:"dst"`
	// Mode, if set, is applied to the copied file as an octal string. State
	// it for anything setuid: inheriting the bit from whatever the upstream
	// makefile happened to do puts the most security-relevant fact about a
	// package somewhere no reviewer of this recipe will see it.
	Mode string `yaml:"mode"`
}

// Link is a symlink the package installs.
type Link struct {
	// Target is what the link points at, as written into the link.
	Target string `yaml:"target"`
	// Path is where the link is created, absolute within the image.
	Path string `yaml:"path"`
}

type Install struct {
	Src  string `yaml:"src"`
	Dst  string `yaml:"dst"`
	Mode string `yaml:"mode"`

	// Owner is the account this file belongs to, as "user" or "user:group",
	// naming an account from this recipe's users: stanza.
	//
	// Without it a package could create a system account and install a
	// config file that account cannot read. That is not hypothetical: FRR
	// ships frr.conf 0640 and runs its daemons as frr, so root-owned config
	// meant ospfd started, found nothing it could read, and reported "OSPF
	// is not enabled" -- a daemon running perfectly with no configuration,
	// which looks like a config error rather than a permission one.
	//
	// Resolved to numeric ids at build time from users:, never from the
	// build host's /etc/passwd, so the package stays reproducible and means
	// the same thing on a machine that has never heard of the account.
	Owner string `yaml:"owner"`
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

// User is a system account the package needs. See nospkg.User for why the
// numbers are fixed rather than allocated.
type User struct {
	Name  string `yaml:"name"`
	UID   int    `yaml:"uid"`
	GID   int    `yaml:"gid"`
	Home  string `yaml:"home"`
	Shell string `yaml:"shell"`
}

// Build describes how the package is compiled.
type Build struct {
	// System is how the package is built: configure, autotools, make, cmake,
	// meson, kernel, kernel-headers, sysroot-libc, or none.
	System string `yaml:"system"`

	// Configure are extra arguments; --prefix is supplied automatically.
	// ${TRIPLE}, ${ARCH}, ${PREFIX} and ${KERNEL_ARCH} are substituted.
	Configure []string `yaml:"configure"`

	// Targets are make targets for system: make. Command-line variable
	// assignments belong here too: make treats VAR=value as an argument, and a
	// build system driven by variables rather than targets is common enough
	// that separating them would be a distinction without a difference.
	Targets []string `yaml:"targets"`

	// Arch overrides parts of this stanza for one architecture, keyed by arch
	// id. Anything it sets replaces the value above; anything it leaves empty
	// keeps it.
	//
	// For the rare package whose build genuinely differs per architecture in a
	// way no substitution expresses. The OpenBCM SDK is the case that forced
	// it: it selects a whole platform definition by name -- x86-64-fc28 or
	// bmw-2_6 -- and those names share nothing with the architecture id, so
	// ${ARCH} cannot reach them. The alternative was to put the SDK's platform
	// names in arch/*/arch.yml, which would be one vendor's build system
	// leaking into the definition of an architecture.
	Arch map[string]ArchBuild `yaml:"arch"`

	// Subdir is where the build runs, relative to the source root. Several
	// projects put the entry point for a given platform in its own directory
	// and expect make to be run there; the OpenBCM SDK is one.
	Subdir string `yaml:"subdir"`

	// Defconfig and Fragment are for system: kernel. The defconfig comes from
	// the kernel tree; the fragment is our own additions, kept small so the
	// decisions are legible rather than buried in thousands of generated lines.
	// Defconfig overrides the architecture's own kernel_defconfig.
	Defconfig string `yaml:"defconfig"`

	// Fragments are applied in order after the defconfig. ${ARCH} is
	// substituted, so one recipe covers every architecture.
	Fragments []string `yaml:"fragments"`

	// OutOfTree runs configure and make in a subdirectory rather than in the
	// source root.
	//
	// Autotools has always supported this; a few projects require it. FRR is
	// one: cross-compiling it means building its clippy code generator for the
	// build machine as well as the target, and it puts that native build in a
	// subdirectory of the source root -- which it refuses to do if the target
	// build is already there. Its configure says so outright and stops.
	OutOfTree bool `yaml:"out_of_tree"`

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

	// Mkdirs are created under the source root before the build runs, for
	// build systems that write into a directory they do not create. The
	// OpenBCM SDK links its application into build/linux/user/common and fails
	// with "cannot open output file" if nothing made it first.
	Mkdirs []string `yaml:"mkdirs"`

	// Stage copies paths out of the build tree into the staging root, for
	// build systems that have no install target of their own. Src is relative
	// to the source directory (after subdir is applied to the build, not to
	// these), Dst is absolute within the stage. A directory is copied whole.
	//
	// The OpenBCM SDK is the case this exists for: it produces static
	// libraries in build/unix-user/<platform> and has nothing that installs
	// them anywhere.
	Stage []StagePath `yaml:"stage"`

	// Prepare runs shell commands in the source directory before the build
	// system is invoked.
	//
	// It exists for a project that ships no configure script. FRR is
	// distributed only as a git tag archive, so autotools has to be run over
	// it first -- and that has to happen with the build machine's autoconf,
	// not the target's, which is why it cannot be expressed as a configure
	// argument. Keep it to generating the build system: anything that shapes
	// what the package contains belongs in the recipe where it is visible.
	Prepare []string `yaml:"prepare"`

	// After runs shell commands in the source directory once the build has
	// finished, before anything is staged.
	//
	// It exists for a build that has to produce something about itself. The
	// case it was added for: a package whose compile-time defines must be
	// known to whatever links against it later. Transcribing those by hand is
	// how an ABI mismatch gets introduced silently -- so the build captures
	// its own, and the consumer reads the file rather than a copy of it.
	After []string `yaml:"after"`

	// Links are symlinks the package installs. Target is what the link points
	// at; Path is where the link goes in the image.
	//
	// This is how a package claims a virtual name without shipping the binary
	// twice: nosd-td2p installs /usr/sbin/nosd-td2p and a /usr/sbin/nosd
	// pointing at it, and everything above the datapath only ever says `nosd`.
	// Copying instead would put 151 MB in the image twice.
	Links []Link `yaml:"links"`

	// CFlags and LDFlags are appended to the cross-compilation flags rather
	// than replacing them.
	//
	// Env can set CFLAGS, but only by replacing the whole value -- which
	// silently drops the staging include and library paths that make a
	// cross-build work at all. These exist so a package can add one flag
	// without having to restate the toolchain.
	CFlags  []string `yaml:"cflags"`
	LDFlags []string `yaml:"ldflags"`

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

	// Depends are needed at RUN time and are installed into the image.
	Depends []string `yaml:"depends"`

	// Users are system accounts this package needs in order to run.
	Users []User `yaml:"users"`

	// BuildDepends are needed only to compile this package. They are staged
	// into the build sysroot and are NOT installed.
	//
	// The distinction is not bookkeeping. The Broadcom SDK is 874 MB of static
	// archives and headers that a datapath daemon links against and an image
	// must never carry: without this it resolved as a runtime dependency and
	// took the image from 31 MB to 794 MB, past the board's 768 MB slot. The
	// slot check caught it, which is the only reason it was not shipped.
	BuildDepends []string `yaml:"build_depends"`
	Provides     []string `yaml:"provides"`
	Conflicts    []string `yaml:"conflicts"`

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
		switch {
		case r.Source.Local != "" && r.Source.URL != "":
			bad("source has both local and url; a recipe builds one or the other")
		case r.Source.Local != "":
			if strings.HasPrefix(r.Source.Local, "/") || strings.Contains(r.Source.Local, "..") {
				bad("source.local %q must be a path inside the repository", r.Source.Local)
			}
			if r.Source.SHA256 != "" {
				bad("source.local does not take a sha256: a directory in this " +
					"repository is covered by its history, and a hash there would " +
					"be a value nobody can recompute")
			}
		case r.Source.URL == "":
			bad("source needs either url or local")
		default:
			if r.Source.SHA256 == "" {
				bad("source.sha256 is required — sources are pinned by hash")
			} else if len(r.Source.SHA256) != 64 {
				bad("source.sha256 must be 64 hex characters, got %d", len(r.Source.SHA256))
			}
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

	// stage: is only honoured by the make build system. Declared anywhere else
	// it does nothing at all, silently -- and the thing people put there is
	// the setuid mode of a privilege helper, which is exactly the stanza that
	// must not quietly have no effect.
	if r.Build != nil && len(r.Build.Stage) > 0 && r.Build.System != "make" {
		bad("build.stage is set but build.system is %q; stage: is only applied by "+
			"the make system, and would be silently ignored here", r.Build.System)
	}

	if r.Build != nil {
		for i, l := range r.Build.Links {
			if l.Target == "" || l.Path == "" {
				bad("build.links[%d]: both target and path are required", i)
			}
			if l.Path != "" && !strings.HasPrefix(l.Path, "/") {
				bad("build.links[%d]: path %q must be absolute within the image", i, l.Path)
			}
		}
	}

	for _, d := range r.BuildDepends {
		for _, r2 := range r.Depends {
			if d == r2 {
				bad("%s is in both depends and build_depends; it would be "+
					"installed as well as staged, which defeats the point of "+
					"saying build_depends", d)
			}
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

// ArchBuild is the per-architecture half of a Build stanza.
//
// Deliberately a small set of fields rather than a second Build: a recipe that
// needs to differ by architecture in more ways than this is a recipe that
// should be read twice before it is made to work.
type ArchBuild struct {
	Targets []string          `yaml:"targets"`
	After   []string          `yaml:"after"`
	Env     map[string]string `yaml:"env"`
}

// ForArch returns this Build with any overrides for the named architecture
// applied.
func (b Build) ForArch(arch string) Build {
	o, ok := b.Arch[arch]
	if !ok {
		return b
	}
	if len(o.Targets) > 0 {
		b.Targets = o.Targets
	}
	if len(o.After) > 0 {
		b.After = o.After
	}
	if len(o.Env) > 0 {
		merged := map[string]string{}
		for k, v := range b.Env {
			merged[k] = v
		}
		for k, v := range o.Env {
			merged[k] = v
		}
		b.Env = merged
	}
	return b
}
