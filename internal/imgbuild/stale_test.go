package imgbuild

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/arch"
	"github.com/salvaged-silicon/nosaic-switch/internal/depsolve"
)

// staleFixture builds a repository with one recipe that is built from a
// directory inside it, plus a package file, and lets the caller decide which is
// newer.
func staleFixture(t *testing.T, srcNewer bool, source string) (Options, []pkgRef, *bytes.Buffer) {
	t.Helper()
	root := t.TempDir()
	pkgs := filepath.Join(root, "packages")
	src := filepath.Join(root, "thing")
	rec := filepath.Join(root, "recipes", "thing")
	for _, d := range []string{pkgs, src, rec} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(rec, "recipe.yml"),
		[]byte("name: thing\nversion: \"1\"\nlicense: Apache-2.0\n"+source), 0o644); err != nil {
		t.Fatal(err)
	}
	pkg := filepath.Join(pkgs, "thing_1_x86_64.nos")
	if err := os.WriteFile(pkg, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	code := filepath.Join(src, "thing.c")
	if err := os.WriteFile(code, []byte("int main(void){return 0;}\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	old := time.Now().Add(-time.Hour)
	// The recipe is source too, so it has to be old whenever the package is
	// meant to look current.
	if err := os.Chtimes(filepath.Join(rec, "recipe.yml"), old, old); err != nil {
		t.Fatal(err)
	}
	if srcNewer {
		if err := os.Chtimes(pkg, old, old); err != nil {
			t.Fatal(err)
		}
	} else {
		if err := os.Chtimes(code, old, old); err != nil {
			t.Fatal(err)
		}
	}
	var log bytes.Buffer
	o := Options{Root: root, PackageDir: pkgs, Log: &log, Arch: &arch.Arch{ID: "x86_64"}}
	refs := []pkgRef{{Pkg: depsolve.Pkg{Name: "thing"}, file: "thing_1_x86_64.nos"}}
	return o, refs, &log
}

// The trap this exists for: edit a directory in this repository, run
// `make image`, and ship the previous binary with nothing saying so. It cost
// three separate hardware diagnoses in one day.
//
// It stops the build rather than warning, because the warning was there for
// that whole day and scrolled past every time.
func TestAStalePackageStopsTheBuild(t *testing.T) {
	o, refs, _ := staleFixture(t, true, "source:\n  local: thing\n")
	err := reportStale(o, refs)
	if err == nil {
		t.Fatal("a package older than its source did not stop the build")
	}
	if !strings.Contains(err.Error(), "make pkg PKG=thing ARCH=x86_64") {
		t.Errorf("the refusal does not say how to fix it: %q", err)
	}
	if !strings.Contains(err.Error(), "--allow-stale") {
		t.Errorf("the refusal does not say how to override it: %q", err)
	}
}

// Building against a package you have not rebuilt is a real thing to want --
// bisecting, or pairing today's image with yesterday's datapath. It must stay
// possible, and it must still say what it is doing.
func TestAllowStaleBuildsAndSaysSo(t *testing.T) {
	o, refs, log := staleFixture(t, true, "source:\n  local: thing\n")
	o.AllowStale = true
	if err := reportStale(o, refs); err != nil {
		t.Fatalf("--allow-stale did not allow a stale package: %v", err)
	}
	if !strings.Contains(log.String(), "--allow-stale") {
		t.Errorf("a stale package was composed silently: %q", log.String())
	}
}

func TestAFreshPackageIsNotReported(t *testing.T) {
	o, refs, log := staleFixture(t, false, "source:\n  local: thing\n")
	if err := reportStale(o, refs); err != nil {
		t.Fatalf("a package newer than its source was refused: %v", err)
	}
	if log.String() != "" {
		t.Errorf("a package newer than its source was reported anyway: %q", log.String())
	}
}

// Most recipes fetch a pinned tarball and have no local source; some have no
// source block at all. Neither is stale and neither may stop the build --
// reading `.Source.Local` on a recipe without a source block panicked the image
// build, and the check's own recover then hid it.
func TestARecipeWithNoSourceBlockIsSkipped(t *testing.T) {
	o, refs, log := staleFixture(t, true, "")
	if err := reportStale(o, refs); err != nil {
		t.Fatalf("a recipe with no source block was refused: %v", err)
	}
	if log.String() != "" {
		t.Errorf("a recipe with no source block produced output: %q", log.String())
	}
}

func TestAnUpstreamSourceIsSkipped(t *testing.T) {
	o, refs, log := staleFixture(t, true,
		"source:\n  url: https://example.invalid/thing.tar.gz\n  sha256: \"\"\n")
	if err := reportStale(o, refs); err != nil {
		t.Fatalf("a recipe built from upstream source was refused: %v", err)
	}
	if log.String() != "" {
		t.Errorf("a recipe built from upstream source produced output: %q", log.String())
	}
}

// The recipe carries the compiler flags, the build targets and what gets
// staged, so changing it changes the binary without touching a line of C.
func TestAChangedRecipeIsStale(t *testing.T) {
	o, refs, _ := staleFixture(t, false, "source:\n  local: thing\n")
	now := time.Now()
	if err := os.Chtimes(filepath.Join(o.Root, "recipes", "thing", "recipe.yml"), now, now); err != nil {
		t.Fatal(err)
	}
	if err := reportStale(o, refs); err == nil {
		t.Error("a recipe newer than its package did not stop the build")
	}
}

// A build leaves its own output in the source tree -- these recipes build in
// place -- and that output is newer than the package by definition. Counting it
// as a source change would make every package stale the moment it was built.
func TestABuildsOwnOutputIsNotASourceChange(t *testing.T) {
	o, refs, log := staleFixture(t, false, "source:\n  local: thing\n")
	src := filepath.Join(o.Root, "thing")
	for _, f := range []string{"thing.o", "libthing.a"} {
		if err := os.WriteFile(filepath.Join(src, f), []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	// A linked binary: executable, and with no extension to recognise it by.
	if err := os.WriteFile(filepath.Join(src, "thing"), []byte("x"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := reportStale(o, refs); err != nil {
		t.Fatalf("a build's own output was read as a source change: %v", err)
	}
	if log.String() != "" {
		t.Errorf("a build's own output produced output: %q", log.String())
	}
}

// nosd-td2p and nosd-tdp both declare `local: datapath` and differ only by
// subdir. td2p compiles td2p/ and common/ and never tdp/, so editing the
// AS5610's daemon must not mark the 7050SX2's package stale. A check that fires
// for something that cannot affect the binary is how a check gets ignored.
func TestAnotherRecipesSubdirIsNotThisOnesSource(t *testing.T) {
	o, refs, log := staleFixture(t, false, "source:\n  local: thing\n  \nbuild:\n  system: make\n  subdir: mine\n")
	// A second recipe over the same tree, building in a different subdir.
	other := filepath.Join(o.Root, "recipes", "other")
	if err := os.MkdirAll(other, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(other, "recipe.yml"),
		[]byte("name: other\nversion: \"1\"\nlicense: Apache-2.0\n"+
			"source:\n  local: thing\n\nbuild:\n  system: make\n  subdir: theirs\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	// Our own source, old. Theirs, edited just now.
	old := time.Now().Add(-time.Hour)
	for _, d := range []string{"mine", "theirs"} {
		if err := os.MkdirAll(filepath.Join(o.Root, "thing", d), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	mine := filepath.Join(o.Root, "thing", "mine", "a.c")
	if err := os.WriteFile(mine, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(mine, old, old); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(o.Root, "thing", "theirs", "b.c"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := reportStale(o, refs); err != nil {
		t.Fatalf("another recipe's subdir was read as this one's source: %v", err)
	}
	if log.String() != "" {
		t.Errorf("another recipe's subdir produced output: %q", log.String())
	}
}

// The shared half of that tree is genuinely shared: datapath/common carries the
// MMIO ordering and the DMA pool, and a change there does affect both daemons.
func TestSharedSourceOutsideEverySubdirIsStale(t *testing.T) {
	o, refs, _ := staleFixture(t, false, "source:\n  local: thing\n\nbuild:\n  system: make\n  subdir: mine\n")
	common := filepath.Join(o.Root, "thing", "common")
	if err := os.MkdirAll(common, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(common, "dmapool.c"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := reportStale(o, refs); err == nil {
		t.Error("a change in the shared source did not stop the build")
	}
}
