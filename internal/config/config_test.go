package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The switch's own setting must beat the image's, because that is the only
// reason the second directory exists.
func TestSiteOverridesImage(t *testing.T) {
	img, site := t.TempDir(), t.TempDir()
	write(t, filepath.Join(img, "asic.conf"), "polled_irq_delay=1000\nshared=from-image\n")
	write(t, filepath.Join(site, "local.conf"), "shared=from-switch\n")

	c, err := LoadFrom(img, site)
	if err != nil {
		t.Fatal(err)
	}
	if v, _ := c.Get("shared"); v != "from-switch" {
		t.Errorf("shared = %q, want the switch's own value to win", v)
	}
	if v, _ := c.Get("polled_irq_delay"); v != "1000" {
		t.Errorf("polled_irq_delay = %q, want the image's value to survive", v)
	}
	for _, s := range c.Settings() {
		if s.Name == "shared" && !s.Site {
			t.Error("an overridden setting must be reported as coming from the switch")
		}
		if s.Name == "polled_irq_delay" && s.Site {
			t.Error("an un-overridden setting must be reported as coming from the image")
		}
	}
}

// Files within a directory layer by name, so the order is stated by the
// filenames rather than by whatever the filesystem returns.
func TestNameOrderWithinADirectory(t *testing.T) {
	img := t.TempDir()
	write(t, filepath.Join(img, "10-base.conf"), "k=first\n")
	write(t, filepath.Join(img, "20-later.conf"), "k=second\n")

	c, err := LoadFrom(img, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	if v, _ := c.Get("k"); v != "second" {
		t.Errorf("k = %q, want the later filename to win", v)
	}
}

// Setting a key twice must leave one line, or the file stops being something a
// person can read and diff.
func TestSetReplacesRatherThanAppends(t *testing.T) {
	site := t.TempDir()
	for _, v := range []string{"one", "two", "three"} {
		val := v
		if err := SetIn(site, "hostname", &val); err != nil {
			t.Fatal(err)
		}
	}
	b, err := os.ReadFile(filepath.Join(site, SiteFile))
	if err != nil {
		t.Fatal(err)
	}
	if n := strings.Count(string(b), "hostname="); n != 1 {
		t.Fatalf("hostname appears %d times, want 1:\n%s", n, b)
	}
	if !strings.Contains(string(b), "hostname=three") {
		t.Errorf("want the last value to be the one kept:\n%s", b)
	}
}

// Removing a setting must fall back to the image rather than to nothing.
func TestUnsetRevealsTheImageDefault(t *testing.T) {
	img, site := t.TempDir(), t.TempDir()
	write(t, filepath.Join(img, "asic.conf"), "speed=10000\n")
	v := "40000"
	if err := SetIn(site, "speed", &v); err != nil {
		t.Fatal(err)
	}
	c, _ := LoadFrom(img, site)
	if got, _ := c.Get("speed"); got != "40000" {
		t.Fatalf("speed = %q before unset, want the override", got)
	}
	if err := SetIn(site, "speed", nil); err != nil {
		t.Fatal(err)
	}
	c, _ = LoadFrom(img, site)
	if got, _ := c.Get("speed"); got != "10000" {
		t.Errorf("speed = %q after unset, want the image's default back", got)
	}
}

// A missing directory is the normal case on a switch nobody has configured and
// on a build host, and must not be an error.
func TestMissingDirectoriesAreNotAnError(t *testing.T) {
	if _, err := LoadFrom("/nonexistent/image", "/nonexistent/site"); err != nil {
		t.Fatalf("want no error for absent directories, got %v", err)
	}
}

// A name that would break the file format has to be refused at the point of
// writing, not discovered when something reads it back.
func TestRefusesNamesThatWouldCorruptTheFile(t *testing.T) {
	site := t.TempDir()
	v := "x"
	for _, bad := range []string{"", "has=equals", "has\nnewline"} {
		if err := SetIn(site, bad, &v); err == nil {
			t.Errorf("SetIn(%q) succeeded, want a refusal", bad)
		}
	}
}

func write(t *testing.T, path, content string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}
