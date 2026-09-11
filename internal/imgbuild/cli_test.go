package imgbuild

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The image and the CLI it ships must agree about what they are. They did not:
// /etc/nosaic/image.json said 0.1.0 while `nosaic version` on the same switch
// said 0.0.0-dev, because this build passed only "-s -w".
func TestTheShippedCLIIsStampedWithTheImagesVersion(t *testing.T) {
	got := cliLDFlags("0.1.0", "abc1234")
	for _, want := range []string{
		"-X " + versionPkg + ".Version=0.1.0",
		"-X " + versionPkg + ".Commit=abc1234",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("the CLI is not stamped with %q: %q", want, got)
		}
	}
	// Still stripped: this rides in every image.
	if !strings.Contains(got, "-s -w") {
		t.Errorf("the CLI is no longer stripped: %q", got)
	}
}

// An offline build with no version should leave the package default, which is
// honest, rather than stamping an empty string that reads as no version at all.
func TestAnUnknownVersionIsLeftAtItsDefault(t *testing.T) {
	got := cliLDFlags("", "")
	if strings.Contains(got, "-X") {
		t.Errorf("an empty version was stamped anyway: %q", got)
	}
}

// A -X flag naming a package that has moved fails silently: the build succeeds
// and the variable keeps its default. That is exactly how the stamp went
// missing without anyone noticing, so the path is checked rather than trusted.
func TestTheVersionPackageIsWhereTheStampSaysItIs(t *testing.T) {
	root, err := filepath.Abs("../..")
	if err != nil {
		t.Fatal(err)
	}
	mod, err := os.ReadFile(filepath.Join(root, "go.mod"))
	if err != nil {
		t.Fatal(err)
	}
	var module string
	for _, line := range strings.Split(string(mod), "\n") {
		if strings.HasPrefix(line, "module ") {
			module = strings.TrimSpace(strings.TrimPrefix(line, "module "))
			break
		}
	}
	if module == "" {
		t.Fatal("no module line in go.mod")
	}
	if !strings.HasPrefix(versionPkg, module+"/") {
		t.Fatalf("versionPkg %q is not in module %q", versionPkg, module)
	}
	dir := filepath.Join(root, strings.TrimPrefix(versionPkg, module+"/"))
	if _, err := os.Stat(dir); err != nil {
		t.Fatalf("versionPkg %q names %s, which does not exist: %v", versionPkg, dir, err)
	}
	// And the variables the stamp sets must still be there to set.
	srcs, _ := filepath.Glob(filepath.Join(dir, "*.go"))
	var all string
	for _, f := range srcs {
		b, _ := os.ReadFile(f)
		all += string(b)
	}
	for _, v := range []string{"Version", "Commit"} {
		if !strings.Contains(all, "var "+v+" =") {
			t.Errorf("%s no longer declares `var %s =`, so -X silently does nothing", versionPkg, v)
		}
	}
}
