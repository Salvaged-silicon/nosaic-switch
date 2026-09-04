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
func TestAStalePackageIsReported(t *testing.T) {
	o, refs, log := staleFixture(t, true, "source:\n  local: thing\n")
	warnStalePackages(o, refs)
	if !strings.Contains(log.String(), "older than its source") {
		t.Errorf("a package older than its source was not reported: %q", log.String())
	}
	if !strings.Contains(log.String(), "make pkg PKG=thing ARCH=x86_64") {
		t.Errorf("the warning does not say how to fix it: %q", log.String())
	}
}

func TestAFreshPackageIsNotReported(t *testing.T) {
	o, refs, log := staleFixture(t, false, "source:\n  local: thing\n")
	warnStalePackages(o, refs)
	if log.String() != "" {
		t.Errorf("a package newer than its source was reported anyway: %q", log.String())
	}
}

// Most recipes fetch a pinned tarball and have no local source; some have no
// source block at all. Neither is stale and neither may crash the build --
// reading `.Source.Local` on a recipe without a source block panicked the image
// build, and the warning's own recover then hid it.
func TestARecipeWithNoSourceBlockIsSkipped(t *testing.T) {
	o, refs, log := staleFixture(t, true, "")
	warnStalePackages(o, refs)
	if log.String() != "" {
		t.Errorf("a recipe with no source block produced output: %q", log.String())
	}
}

func TestAnUpstreamSourceIsSkipped(t *testing.T) {
	o, refs, log := staleFixture(t, true,
		"source:\n  url: https://example.invalid/thing.tar.gz\n  sha256: \"\"\n")
	warnStalePackages(o, refs)
	if log.String() != "" {
		t.Errorf("a recipe built from upstream source produced output: %q", log.String())
	}
}
