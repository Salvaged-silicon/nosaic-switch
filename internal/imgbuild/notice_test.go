package imgbuild

import (
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func comps() []component {
	return []component{
		{Name: "zlib", Version: "1.3.1", License: "Zlib", Redistributable: true},
		{Name: "openbcm", Version: "6.5.24", License: "Broadcom-OpenBCM", Redistributable: true},
	}
}

// The obligation this exists for: an image containing a vendor SDK must
// reproduce that vendor's notice. An image that ships the code and not the
// notice is the one case where the build has done something its licence does
// not permit -- and that is exactly what it did before this existed.
func TestNoticeNamesEveryComponentAndLicense(t *testing.T) {
	dir := t.TempDir()
	if _, err := writeAttribution(dir, "0.0.0", "b", "x86_64", comps(), io.Discard); err != nil {
		t.Fatal(err)
	}
	got, err := readNotice(dir)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"zlib-1.3.1", "Zlib", "openbcm-6.5.24", "Broadcom-OpenBCM"} {
		if !strings.Contains(got, want) {
			t.Errorf("the NOTICE does not mention %q:\n%s", want, got)
		}
	}
}

// A component with no declared licence is called out rather than shown blank,
// which reads as "no licence applies".
func TestNoticeMarksUndeclaredLicenses(t *testing.T) {
	dir := t.TempDir()
	c := []component{{Name: "mystery", Version: "1", Redistributable: true}}
	if _, err := writeAttribution(dir, "0.0.0", "b", "x86_64", c, io.Discard); err != nil {
		t.Fatal(err)
	}
	got, _ := readNotice(dir)
	if !strings.Contains(got, "UNDECLARED") {
		t.Errorf("an undeclared licence should be named as such:\n%s", got)
	}
}

// Non-redistributable content makes the image unpublishable, and says so in
// both halves. Building it is legitimate; publishing it is not, and something
// has to be able to tell the difference without reading prose.
func TestNonRedistributableMarksImageUnpublishable(t *testing.T) {
	dir := t.TempDir()
	c := append(comps(), component{Name: "vendor-blob", Version: "1", License: "proprietary"})
	pub, err := writeAttribution(dir, "0.0.0", "b", "x86_64", c, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if pub {
		t.Error("an image containing a non-redistributable component must not be publishable")
	}
	got, _ := readNotice(dir)
	if !strings.Contains(got, "vendor-blob") || !strings.Contains(got, "NOT REDISTRIBUTABLE") {
		t.Errorf("the NOTICE should name what cannot be redistributed:\n%s", got)
	}

	b, err := os.ReadFile(filepath.Join(dir, strings.TrimPrefix(sbomPath, "/")))
	if err != nil {
		t.Fatal(err)
	}
	var doc sbom
	if err := json.Unmarshal(b, &doc); err != nil {
		t.Fatalf("the SBOM is not valid JSON: %v", err)
	}
	if doc.Publishable {
		t.Error("the SBOM should record the image as not publishable")
	}
	if len(doc.Components) != len(c) {
		t.Errorf("the SBOM lists %d components, want %d", len(doc.Components), len(c))
	}
}

// An all-clear image says so, so that "publishable" is a positive statement
// rather than the absence of a warning.
func TestPublishableWhenEverythingIsRedistributable(t *testing.T) {
	dir := t.TempDir()
	pub, err := writeAttribution(dir, "0.0.0", "b", "x86_64", comps(), io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if !pub {
		t.Error("everything is redistributable; the image should be publishable")
	}
}
