package arch

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func load(t *testing.T, id, body string) *Arch {
	t.Helper()
	root := t.TempDir()
	dir := filepath.Join(root, "arch", id)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(dir, "arch.yml")
	if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	a, err := Load(p)
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	return a
}

func hasErr(errs []string, substr string) bool {
	for _, e := range errs {
		if strings.Contains(e, substr) {
			return true
		}
	}
	return false
}

const valid = `
id: x86_64
name: x86-64
triple: x86_64-nosaic-linux-gnu
endian: little
bits: 64
kernel_arch: x86
ctng_sample: x86_64-unknown-linux-gnu
qemu: ""
status: supported
`

func TestValidArch(t *testing.T) {
	if errs := load(t, "x86_64", valid).Validate(); len(errs) != 0 {
		t.Fatalf("expected no errors, got %v", errs)
	}
}

// The vendor field is what makes an accidental host-compiler build visible as
// a triple mismatch instead of something that works until it doesn't.
func TestTripleMustCarryVendor(t *testing.T) {
	a := load(t, "x86_64", strings.Replace(valid,
		"x86_64-nosaic-linux-gnu", "x86_64-unknown-linux-gnu", 1))
	if !hasErr(a.Validate(), "nosaic vendor field") {
		t.Fatalf("a triple without the vendor field should be an error, got %v", a.Validate())
	}
}

func TestTripleMustMatchID(t *testing.T) {
	a := load(t, "x86_64", strings.Replace(valid,
		"triple: x86_64-nosaic-linux-gnu", "triple: aarch64-nosaic-linux-gnu", 1))
	if !hasErr(a.Validate(), "does not start with the arch id") {
		t.Fatalf("a triple disagreeing with the id should be an error, got %v", a.Validate())
	}
}

func TestIDMustMatchDirectory(t *testing.T) {
	a := load(t, "amd64", valid) // directory amd64, id x86_64
	if !hasErr(a.Validate(), "does not match its directory") {
		t.Fatalf("id/directory mismatch should be an error, got %v", a.Validate())
	}
}

// Seeding from an upstream sample is what makes a defconfig reproducible
// rather than a hand-tuned artifact nobody dares regenerate.
func TestSampleRequired(t *testing.T) {
	a := load(t, "x86_64", strings.Replace(valid,
		"ctng_sample: x86_64-unknown-linux-gnu", `ctng_sample: ""`, 1))
	if !hasErr(a.Validate(), "ctng_sample is required") {
		t.Fatalf("missing sample should be an error, got %v", a.Validate())
	}
}

func TestBadEndianRejected(t *testing.T) {
	a := load(t, "x86_64", strings.Replace(valid, "endian: little", "endian: middle", 1))
	if !hasErr(a.Validate(), "endian") {
		t.Fatalf("nonsense endianness should be an error, got %v", a.Validate())
	}
}

func TestNative(t *testing.T) {
	if !load(t, "x86_64", valid).Native() {
		t.Fatal("empty qemu means native")
	}
	a := load(t, "x86_64", strings.Replace(valid, `qemu: ""`, "qemu: qemu-x86_64-static", 1))
	if a.Native() {
		t.Fatal("a named emulator means not native")
	}
}

func TestUnknownFieldRejected(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "arch", "x86_64")
	_ = os.MkdirAll(dir, 0o755)
	p := filepath.Join(dir, "arch.yml")
	_ = os.WriteFile(p, []byte(valid+"\ntripple: oops\n"), 0o644)
	if _, err := Load(p); err == nil {
		t.Fatal("expected a typo'd field to be rejected")
	}
}
