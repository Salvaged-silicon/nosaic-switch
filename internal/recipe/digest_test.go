package recipe

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func digest(t *testing.T, dir string) string {
	t.Helper()
	d, err := Digest(dir)
	if err != nil {
		t.Fatal(err)
	}
	return d
}

// What the digest is for: content changes it, and nothing else does.
func TestDigestFollowsContentOnly(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "recipe.yml"), "name: thing\n")
	writeFile(t, filepath.Join(dir, "patches", "0001-a.patch"), "a\n")
	base := digest(t, dir)

	// A checkout moves file times and changes nothing.
	now := time.Now().Add(time.Hour)
	if err := os.Chtimes(filepath.Join(dir, "recipe.yml"), now, now); err != nil {
		t.Fatal(err)
	}
	if digest(t, dir) != base {
		t.Error("a new file time changed the digest")
	}

	// Prose cannot change a package.
	writeFile(t, filepath.Join(dir, "patches", "README.md"), "why\n")
	if digest(t, dir) != base {
		t.Error("a README changed the digest")
	}

	writeFile(t, filepath.Join(dir, "patches", "0002-b.patch"), "b\n")
	withPatch := digest(t, dir)
	if withPatch == base {
		t.Error("a new patch did not change the digest")
	}

	// A rename is a change even with identical content: patches apply in
	// name order.
	if err := os.Rename(filepath.Join(dir, "patches", "0002-b.patch"),
		filepath.Join(dir, "patches", "0000-b.patch")); err != nil {
		t.Fatal(err)
	}
	if digest(t, dir) == withPatch {
		t.Error("renaming a patch did not change the digest")
	}
}

// A line moved from the end of one file to the start of the next is a
// different recipe, though the bytes laid end to end are the same.
func TestDigestKeepsFilesApart(t *testing.T) {
	a, b := t.TempDir(), t.TempDir()
	writeFile(t, filepath.Join(a, "1"), "xy")
	writeFile(t, filepath.Join(a, "2"), "z")
	writeFile(t, filepath.Join(b, "1"), "x")
	writeFile(t, filepath.Join(b, "2"), "yz")
	if digest(t, a) == digest(t, b) {
		t.Error("moving bytes between files did not change the digest")
	}
}
