// Package profile loads a base system profile.
//
// A profile is a tier — full, slim or minimal — describing which packages make
// a bootable system and which init runs it. The tiers share recipes and differ
// only in package set and init, which is why recipes declare services
// abstractly rather than shipping unit files.
package profile

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"gopkg.in/yaml.v3"
)

var validInit = []string{"systemd", "s6", "busybox"}

// Profile is one base system tier.
type Profile struct {
	Name     string   `yaml:"name"`
	Init     string   `yaml:"init"`
	Packages []string `yaml:"packages"`
	Slots    int      `yaml:"slots"`

	// Privilege is how the login account becomes root on this tier. It is per
	// profile rather than global because the tiers genuinely differ: sudo is
	// the expected answer and does not fit a profile built for boards with
	// little flash, where doas does the same job in a fraction of the size.
	//
	// Empty means "take the default from base/identity.yml", so a profile only
	// states this when it differs.
	Privilege   string `yaml:"privilege"`
	Description string `yaml:"description"`

	Path string `yaml:"-"`
}

// Load reads base/<name>.yml.
func Load(root, name string) (*Profile, error) {
	p := filepath.Join(root, "base", name+".yml")
	b, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	var pr Profile
	dec := yaml.NewDecoder(strings.NewReader(string(b)))
	dec.KnownFields(true)
	if err := dec.Decode(&pr); err != nil {
		return nil, fmt.Errorf("%s: %w", p, err)
	}
	pr.Path = p
	return &pr, nil
}

// LoadAll reads every profile. identity.yml is not one.
func LoadAll(root string) ([]*Profile, error) {
	matches, err := filepath.Glob(filepath.Join(root, "base", "*.yml"))
	if err != nil {
		return nil, err
	}
	var out []*Profile
	for _, m := range matches {
		name := strings.TrimSuffix(filepath.Base(m), ".yml")
		if name == "identity" {
			continue
		}
		p, err := Load(root, name)
		if err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, nil
}

// Validate returns every problem with this profile.
func (p *Profile) Validate() []string {
	var errs []string
	bad := func(f string, a ...any) { errs = append(errs, fmt.Sprintf(f, a...)) }

	if p.Name == "" {
		bad("name is required")
	} else if base := strings.TrimSuffix(filepath.Base(p.Path), ".yml"); base != p.Name {
		bad("name %q does not match its file %q", p.Name, base)
	}
	if !oneOf(p.Init, validInit) {
		bad("init %q must be one of %s", p.Init, strings.Join(validInit, ", "))
	}
	if len(p.Packages) == 0 {
		bad("packages is empty: a profile with no packages produces nothing bootable")
	}
	// Two slots is what makes an upgrade reversible. One is legitimate on a
	// board with no room for two, but it must be a stated choice rather than
	// an omission, because it silently removes rollback.
	// "none" is deliberately not accepted here. A profile with no privilege
	// path is a switch that cannot touch its own hardware -- the platform HAL
	// needs root to reach the board controller -- and it is a state that has
	// already shipped once by accident, because base/identity.yml declared
	// sudo and nothing packaged it.
	if p.Privilege != "" && !oneOf(p.Privilege, []string{"sudo", "doas"}) {
		bad("privilege %q must be sudo or doas", p.Privilege)
	}
	if p.Slots != 1 && p.Slots != 2 {
		bad("slots must be 1 or 2, got %d — 1 means upgrades are not atomic and cannot roll back", p.Slots)
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
