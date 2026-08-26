// Package arch parses and validates CPU architecture definitions.
//
// One directory per architecture under arch/. The set of architectures is
// derived by scanning them, not listed anywhere central, so adding a CPU is a
// directory — the same rule board ports follow.
package arch

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"gopkg.in/yaml.v3"
)

// Status describes an architecture's role.
//
//	supported — a board targets it
//	canary    — cross-built in CI and never booted, purely so that the
//	            architecture seam fails loudly the moment something
//	            x86-specific is assumed
//	planned   — declared, not yet built
var validStatus = []string{"supported", "canary", "planned"}

var validEndian = []string{"little", "big"}

// Arch is one CPU architecture.
type Arch struct {
	ID     string `yaml:"id"`
	Name   string `yaml:"name"`
	Triple string `yaml:"triple"`
	Endian string `yaml:"endian"`
	Bits   int    `yaml:"bits"`

	KernelArch string `yaml:"kernel_arch"`

	// CtngSample is the upstream crosstool-NG sample the defconfig is seeded
	// from. Recorded so the config can be regenerated rather than being a
	// hand-tuned artifact nobody dares touch.
	CtngSample string `yaml:"ctng_sample"`

	// QEMU is the user-mode emulator for running this architecture's binaries
	// on the build host. Empty means native.
	QEMU string `yaml:"qemu"`

	Status string `yaml:"status"`
	Notes  string `yaml:"notes"`

	Path string `yaml:"-"`
}

// Native reports whether binaries run on the build host without emulation.
func (a *Arch) Native() bool { return a.QEMU == "" }

// Load reads a single arch.yml.
func Load(path string) (*Arch, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var a Arch
	dec := yaml.NewDecoder(strings.NewReader(string(b)))
	dec.KnownFields(true)
	if err := dec.Decode(&a); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	a.Path = path
	return &a, nil
}

// LoadAll scans arch/ for architecture definitions.
func LoadAll(root string) ([]*Arch, error) {
	dir := filepath.Join(root, "arch")
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []*Arch
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		p := filepath.Join(dir, e.Name(), "arch.yml")
		if _, err := os.Stat(p); os.IsNotExist(err) {
			continue
		}
		a, err := Load(p)
		if err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out, nil
}

// Validate returns every problem with this architecture definition.
func (a *Arch) Validate() []string {
	var errs []string
	bad := func(f string, v ...any) { errs = append(errs, fmt.Sprintf(f, v...)) }

	if a.ID == "" {
		bad("id is required")
	} else if dir := filepath.Base(filepath.Dir(a.Path)); dir != a.ID {
		bad("id %q does not match its directory %q", a.ID, dir)
	}

	if a.Triple == "" {
		bad("triple is required")
	} else {
		// A NOSaic toolchain carries its own vendor field, so a binary built
		// by the host compiler is a visible triple mismatch rather than
		// something that works until it quietly doesn't.
		if !strings.Contains(a.Triple, "-nosaic-") {
			bad("triple %q should carry the nosaic vendor field (<cpu>-nosaic-linux-gnu)", a.Triple)
		}
		if a.ID != "" && !strings.HasPrefix(a.Triple, a.ID+"-") {
			bad("triple %q does not start with the arch id %q", a.Triple, a.ID)
		}
	}

	if !oneOf(a.Endian, validEndian) {
		bad("endian %q must be one of %s", a.Endian, strings.Join(validEndian, ", "))
	}
	if a.Bits != 32 && a.Bits != 64 {
		bad("bits must be 32 or 64, got %d", a.Bits)
	}
	if a.KernelArch == "" {
		bad("kernel_arch is required")
	}
	if a.CtngSample == "" {
		bad("ctng_sample is required — defconfigs are seeded from an upstream sample, not hand-written")
	}
	if !oneOf(a.Status, validStatus) {
		bad("status %q must be one of %s", a.Status, strings.Join(validStatus, ", "))
	}

	return errs
}

func oneOf(v string, valid []string) bool {
	for _, s := range valid {
		if v == s {
			return true
		}
	}
	return false
}
