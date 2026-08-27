package recipe

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func write(t *testing.T, body string) string {
	t.Helper()
	dir := t.TempDir()
	p := filepath.Join(dir, "recipe.yml")
	if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func loadOK(t *testing.T, body string) *Recipe {
	t.Helper()
	r, err := Load(write(t, body))
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	return r
}

// hasErr reports whether any validation error mentions substr.
func hasErr(errs []string, substr string) bool {
	for _, e := range errs {
		if strings.Contains(e, substr) {
			return true
		}
	}
	return false
}

const minimal = `
name: json-c
version: "0.17"
license: MIT
redistributable: true
`

func TestMinimalRecipeIsValid(t *testing.T) {
	if errs := loadOK(t, minimal).Validate(); len(errs) != 0 {
		t.Fatalf("expected no errors, got %v", errs)
	}
}

// The licensing gate must refuse omission rather than assume a default. A
// component with no stated license is the thing that eventually gets
// published by mistake.
func TestLicenseIsRequired(t *testing.T) {
	errs := loadOK(t, `
name: mystery
version: "1.0"
redistributable: true
`).Validate()
	if !hasErr(errs, "license is required") {
		t.Fatalf("missing license should be an error, got %v", errs)
	}
}

func TestRedistributableMustBeExplicit(t *testing.T) {
	errs := loadOK(t, `
name: mystery
version: "1.0"
license: MIT
`).Validate()
	if !hasErr(errs, "redistributable must be set explicitly") {
		t.Fatalf("unset redistributable should be an error, got %v", errs)
	}
}

// Distinguishing unset from false is the whole reason Redistributable is a
// pointer; if that regresses, an omitted decision silently becomes "false"
// and the recipe passes for the wrong reason.
func TestRedistributableFalseIsValid(t *testing.T) {
	r := loadOK(t, `
name: vendor-blob
version: "1.0"
license: Vendor-Proprietary
redistributable: false
`)
	if errs := r.Validate(); len(errs) != 0 {
		t.Fatalf("explicit false is a legitimate answer, got %v", errs)
	}
	if r.Redistributable == nil || *r.Redistributable {
		t.Fatal("expected redistributable to parse as explicit false")
	}
}

// A virtual name must be exclusive. Without this, nosd-td2 and nosd-helix4
// can co-install and fight over one chip.
func TestProvidesRequiresConflicts(t *testing.T) {
	errs := loadOK(t, `
name: nosd-td2
version: "0.1"
license: Apache-2.0
redistributable: true
provides: [nosd]
`).Validate()
	if !hasErr(errs, "must be exclusive") {
		t.Fatalf("provides without conflicts should be an error, got %v", errs)
	}
}

func TestProvidesWithConflictsIsValid(t *testing.T) {
	errs := loadOK(t, `
name: nosd-td2
version: "0.1"
license: Apache-2.0
redistributable: true
provides: [nosd]
conflicts: [nosd]
`).Validate()
	if len(errs) != 0 {
		t.Fatalf("expected no errors, got %v", errs)
	}
}

// Sources are pinned by hash so that a build is reproducible and an upstream
// cannot change under it.
func TestSourceMustBePinned(t *testing.T) {
	errs := loadOK(t, `
name: frr
version: "10.2"
license: GPL-2.0-or-later
redistributable: true
source:
  url: https://example.invalid/frr-10.2.tar.gz
`).Validate()
	if !hasErr(errs, "sha256 is required") {
		t.Fatalf("unpinned source should be an error, got %v", errs)
	}
}

func TestShortHashRejected(t *testing.T) {
	errs := loadOK(t, `
name: frr
version: "10.2"
license: GPL-2.0-or-later
redistributable: true
source:
  url: https://example.invalid/frr-10.2.tar.gz
  sha256: deadbeef
`).Validate()
	if !hasErr(errs, "64 hex characters") {
		t.Fatalf("truncated hash should be an error, got %v", errs)
	}
}

// An unknown key is nearly always a typo. Accepting it silently means the
// setting the author intended never takes effect.
func TestUnknownFieldRejected(t *testing.T) {
	_, err := Load(write(t, minimal+"\nredistributible: true\n"))
	if err == nil {
		t.Fatal("expected a typo'd field to be rejected")
	}
}

func TestInstallDestMustBeAbsolute(t *testing.T) {
	errs := loadOK(t, `
name: frr
version: "10.2"
license: GPL-2.0-or-later
redistributable: true
install:
  - {src: usr/sbin/zebra, dst: usr/sbin/zebra, mode: "0755"}
`).Validate()
	if !hasErr(errs, "absolute path") {
		t.Fatalf("relative dst should be an error, got %v", errs)
	}
}

// The rule that keeps the minimal profile alive: a recipe may not hand-write
// an init file, because a systemd unit does nothing on a box running s6.
func TestInitOwnedPathsRejected(t *testing.T) {
	for _, dst := range []string{
		"/etc/systemd/system/zebra.service",
		"/usr/lib/systemd/system/zebra.service",
		"/lib/systemd/system/zebra.service",
		"/etc/init.d/zebra",
		"/etc/s6-rc/source/zebra/run",
	} {
		r := loadOK(t, `
name: frr
version: "10.2"
license: GPL-2.0-or-later
redistributable: true
install:
  - {src: unit, dst: "`+dst+`", mode: "0644"}
`)
		if !hasErr(r.Validate(), "owned by an init system") {
			t.Errorf("%s should be rejected, got %v", dst, r.Validate())
		}
	}
}

func TestServiceValidatedThroughGenerator(t *testing.T) {
	r := loadOK(t, `
name: frr
version: "10.2"
license: GPL-2.0-or-later
redistributable: true
services:
  - {name: zebra, exec: "zebra -f /etc/frr/zebra.conf"}
`)
	if !hasErr(r.Validate(), "absolute path") {
		t.Fatalf("a relative exec should be rejected, got %v", r.Validate())
	}
}
