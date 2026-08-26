// Package board parses and validates board ports.
//
// A board port is one self-contained directory under platform/. Nothing
// central lists the supported boards: the catalog is derived by scanning
// these directories, so adding a switch means adding a folder and touching
// no shared file.
package board

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"gopkg.in/yaml.v3"
)

// Status is how far a port has got. Only "production" boards are advertised
// in the README — the project claims support for what works, not what is
// being attempted.
var validStatus = []string{"planned", "bringup", "experimental", "production"}

// validProfile matches the tiers in base/. See docs/DESIGN.md.
var validProfile = []string{"full", "slim", "minimal"}

// Board is one physical switch or router.
type Board struct {
	ID     string `yaml:"id"`
	Vendor string `yaml:"vendor"`
	Model  string `yaml:"model"`

	// The orthogonal axes. Each names a directory: arch/<arch>,
	// boot/<boot>. asic selects the nosd provider.
	Arch string `yaml:"arch"`
	ASIC string `yaml:"asic"`
	Boot string `yaml:"boot"`

	Profile string `yaml:"profile"`
	Kernel  string `yaml:"kernel"`
	Status  string `yaml:"status"`

	Notes string `yaml:"notes"`

	Path string `yaml:"-"`
}

// Load reads a single board.yml.
func Load(path string) (*Board, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var brd Board
	dec := yaml.NewDecoder(strings.NewReader(string(b)))
	dec.KnownFields(true)
	if err := dec.Decode(&brd); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	brd.Path = path
	return &brd, nil
}

// LoadAll scans platform/ for board ports. TEMPLATE is skipped: it is the
// scaffold contributors copy, not a board.
func LoadAll(root string) ([]*Board, error) {
	dir := filepath.Join(root, "platform")
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []*Board
	for _, e := range entries {
		if !e.IsDir() || e.Name() == "TEMPLATE" {
			continue
		}
		p := filepath.Join(dir, e.Name(), "board.yml")
		if _, err := os.Stat(p); os.IsNotExist(err) {
			continue
		}
		brd, err := Load(p)
		if err != nil {
			return nil, err
		}
		out = append(out, brd)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out, nil
}

// Validate returns every problem with this board port.
func (b *Board) Validate(root string) []string {
	var errs []string
	bad := func(f string, a ...any) { errs = append(errs, fmt.Sprintf(f, a...)) }

	if b.ID == "" {
		bad("id is required")
	} else if dir := filepath.Base(filepath.Dir(b.Path)); dir != b.ID {
		bad("id %q does not match its directory %q", b.ID, dir)
	}
	if b.Arch == "" {
		bad("arch is required")
	}
	if b.ASIC == "" {
		bad("asic is required")
	}
	if b.Boot == "" {
		bad("boot is required")
	}
	if !oneOf(b.Status, validStatus) {
		bad("status %q must be one of %s", b.Status, strings.Join(validStatus, ", "))
	}
	if !oneOf(b.Profile, validProfile) {
		bad("profile %q must be one of %s", b.Profile, strings.Join(validProfile, ", "))
	}

	// The axes are directories. A board naming one that does not exist is a
	// port that cannot build, and saying so here is cheaper than finding out
	// during an image build.
	if b.Arch != "" {
		if _, err := os.Stat(filepath.Join(root, "arch", b.Arch)); os.IsNotExist(err) {
			bad("arch %q has no arch/%s directory", b.Arch, b.Arch)
		}
	}
	if b.Boot != "" {
		if _, err := os.Stat(filepath.Join(root, "boot", b.Boot)); os.IsNotExist(err) {
			bad("boot %q has no boot/%s directory", b.Boot, b.Boot)
		}
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
