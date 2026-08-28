package nospkg

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

// incompressible returns n bytes that gzip cannot shrink, so that tests about
// payload size and corruption are testing what they say they are.
func incompressible(n int) string {
	b := make([]byte, n)
	x := uint32(0x12345678)
	for i := range b {
		x ^= x << 13
		x ^= x >> 17
		x ^= x << 5
		b[i] = byte(x)
	}
	return string(b)
}

func srcFile(t *testing.T, name, content string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(p, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func manifest() *Manifest {
	return &Manifest{
		Name:            "json-c",
		Version:         "0.17",
		Arch:            "x86_64",
		License:         "MIT",
		Redistributable: true,
		Build:           BuildInfo{Epoch: 1000000000},
	}
}

func build(t *testing.T, m *Manifest, entries []Entry) []byte {
	t.Helper()
	var buf bytes.Buffer
	if err := Build(&buf, m, entries); err != nil {
		t.Fatalf("build: %v", err)
	}
	return buf.Bytes()
}

func TestRoundTrip(t *testing.T) {
	src := srcFile(t, "libjson-c.so", "ELF and so on")
	pkg := build(t, manifest(), []Entry{
		{Dst: "/usr/lib/libjson-c.so", Src: src, Mode: 0o755},
	})

	m, err := Verify(bytes.NewReader(pkg))
	if err != nil {
		t.Fatalf("verify: %v", err)
	}
	if len(m.Files) != 1 || m.Files[0].Path != "/usr/lib/libjson-c.so" {
		t.Fatalf("unexpected files: %+v", m.Files)
	}
	if m.Files[0].SHA256 == "" {
		t.Fatal("a regular file should carry a digest")
	}
	if m.Filename() != "json-c_0.17_x86_64.nos" {
		t.Fatalf("filename = %q", m.Filename())
	}
}

// The whole claim of the format. Two builds of the same inputs must be
// byte-identical, or "the image you built is the image I built" is unverifiable.
func TestReproducible(t *testing.T) {
	src := srcFile(t, "a", "content")
	entries := []Entry{{Dst: "/usr/bin/a", Src: src, Mode: 0o755}}

	first := build(t, manifest(), entries)
	second := build(t, manifest(), entries)

	if !bytes.Equal(first, second) {
		t.Fatalf("two builds differed: %d vs %d bytes", len(first), len(second))
	}
}

// The order files were discovered in must not leak into the archive — a
// directory walk on another machine may yield a different order.
func TestEntryOrderDoesNotMatter(t *testing.T) {
	dir := t.TempDir()
	for _, n := range []string{"a", "b", "c"} {
		if err := os.WriteFile(filepath.Join(dir, n), []byte(n), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	mk := func(names ...string) []Entry {
		var es []Entry
		for _, n := range names {
			es = append(es, Entry{Dst: "/usr/bin/" + n, Src: filepath.Join(dir, n), Mode: 0o755})
		}
		return es
	}
	forward := build(t, manifest(), mk("a", "b", "c"))
	reverse := build(t, manifest(), mk("c", "b", "a"))

	if !bytes.Equal(forward, reverse) {
		t.Fatal("entry order changed the output; entries are not being sorted")
	}
}

// A build must not embed the wall clock, or nothing is reproducible across time.
func TestEpochIsUsedNotTheClock(t *testing.T) {
	src := srcFile(t, "a", "content")
	entries := []Entry{{Dst: "/usr/bin/a", Src: src, Mode: 0o755}}

	m1 := manifest()
	m1.Build.Epoch = 1000000000
	m2 := manifest()
	m2.Build.Epoch = 2000000000

	if bytes.Equal(build(t, m1, entries), build(t, m2, entries)) {
		t.Fatal("different epochs produced identical archives; the epoch is being ignored")
	}
}

func TestTamperedPayloadIsRejected(t *testing.T) {
	// Incompressible, so the payload is genuinely large and a corrupted byte
	// lands in real data rather than in the tar's trailing padding.
	src := srcFile(t, "a", incompressible(64*1024))
	pkg := build(t, manifest(), []Entry{{Dst: "/usr/bin/a", Src: src, Mode: 0o755}})

	// Corrupt several positions across the payload; each must be caught.
	for _, frac := range []float64{0.5, 0.7, 0.9} {
		tampered := append([]byte(nil), pkg...)
		at := int(float64(len(pkg)) * frac)
		tampered[at] ^= 0xff
		if _, err := Verify(bytes.NewReader(tampered)); err == nil {
			t.Fatalf("a payload corrupted at %d/%d verified successfully", at, len(pkg))
		}
	}
}

// Reading a manifest must not require touching the payload: on a switch,
// discovering a package is for the wrong CPU should not cost a decompression.
func TestManifestReadsWithoutPayload(t *testing.T) {
	src := srcFile(t, "a", incompressible(256*1024))
	pkg := build(t, manifest(), []Entry{{Dst: "/usr/bin/a", Src: src, Mode: 0o755}})

	// Truncate to a small head of the file: enough for the manifest, nowhere
	// near enough for a 256 KiB payload.
	head := 4096
	if len(pkg) <= head*4 {
		t.Fatalf("payload too small for this test to mean anything (%d bytes)", len(pkg))
	}
	m, err := ReadManifest(bytes.NewReader(pkg[:head]))
	if err != nil {
		t.Fatalf("manifest should be readable from the head of the file: %v", err)
	}
	if m.Name != "json-c" {
		t.Fatalf("name = %q", m.Name)
	}
}

func TestArchGuard(t *testing.T) {
	m := manifest()
	if err := m.CanInstall("powerpc"); err == nil {
		t.Fatal("an x86_64 package must not install on powerpc")
	}
	if err := m.CanInstall("x86_64"); err != nil {
		t.Fatalf("matching arch should install: %v", err)
	}
	m.Arch = ArchAny
	if err := m.CanInstall("powerpc"); err != nil {
		t.Fatalf("an arch-independent package installs anywhere: %v", err)
	}
}

func TestSymlinksAndDirs(t *testing.T) {
	src := srcFile(t, "real", "content")
	pkg := build(t, manifest(), []Entry{
		{Dst: "/usr/lib", Dir: true, Mode: 0o755},
		{Dst: "/usr/lib/libfoo.so.1", Src: src, Mode: 0o755},
		{Dst: "/usr/lib/libfoo.so", Link: "libfoo.so.1"},
	})
	m, err := Verify(bytes.NewReader(pkg))
	if err != nil {
		t.Fatalf("verify: %v", err)
	}
	if len(m.Files) != 3 {
		t.Fatalf("expected 3 entries, got %d", len(m.Files))
	}
	for _, f := range m.Files {
		if f.Path == "/usr/lib/libfoo.so" && f.Link != "libfoo.so.1" {
			t.Fatalf("symlink target not recorded: %+v", f)
		}
	}
}

// The licensing gate starts here: a package with no stated license cannot be
// built at all, so it can never reach an image.
func TestLicenseRequired(t *testing.T) {
	m := manifest()
	m.License = ""
	var buf bytes.Buffer
	if err := Build(&buf, m, nil); err == nil {
		t.Fatal("a package with no license should not build")
	}
}

func TestRelativeDestinationRejected(t *testing.T) {
	src := srcFile(t, "a", "content")
	var buf bytes.Buffer
	err := Build(&buf, manifest(), []Entry{{Dst: "usr/bin/a", Src: src, Mode: 0o755}})
	if err == nil {
		t.Fatal("a relative install path should be rejected")
	}
}

// Installing a real file where an earlier package left a symlink must replace
// the link, not write through it.
//
// This is how the slim profile's first image came out unbootable. busybox
// installs /bin/ip as a symlink to busybox; iproute2 installs a real /bin/ip.
// Extraction opened the path with O_TRUNC, which follows a symlink, so
// iproute2's binary landed on /bin/busybox. The image then had the ip binary
// named busybox, and the box panicked at boot with "Failed to execute /init".
//
// Nothing about that failure points at package extraction, which is why it is
// pinned here.
func TestInstallingOverASymlinkDoesNotWriteThroughIt(t *testing.T) {
	dst := t.TempDir()

	// First package: a real binary plus a symlink pointing at it.
	first := build(t, manifest(), []Entry{
		{Dst: "/bin/busybox", Src: srcFile(t, "busybox", "THE REAL BUSYBOX"), Mode: 0o755},
		{Dst: "/bin/ip", Link: "busybox"},
	})
	if _, err := Extract(writeTemp(t, first), dst); err != nil {
		t.Fatalf("extract first: %v", err)
	}

	// Second package: a real file at the path the symlink occupies.
	second := build(t, manifest(), []Entry{
		{Dst: "/bin/ip", Src: srcFile(t, "ip", "IPROUTE2 IP BINARY"), Mode: 0o755},
	})
	if _, err := Extract(writeTemp(t, second), dst); err != nil {
		t.Fatalf("extract second: %v", err)
	}

	got, err := os.ReadFile(filepath.Join(dst, "bin", "busybox"))
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "THE REAL BUSYBOX" {
		t.Errorf("/bin/busybox was overwritten through the symlink: got %q", got)
	}
	ip, err := os.ReadFile(filepath.Join(dst, "bin", "ip"))
	if err != nil {
		t.Fatal(err)
	}
	if string(ip) != "IPROUTE2 IP BINARY" {
		t.Errorf("/bin/ip is %q, want the second package's file", ip)
	}
	// And it must be a real file now, not still a link.
	fi, err := os.Lstat(filepath.Join(dst, "bin", "ip"))
	if err != nil {
		t.Fatal(err)
	}
	if fi.Mode()&os.ModeSymlink != 0 {
		t.Error("/bin/ip is still a symlink")
	}
}

func writeTemp(t *testing.T, pkg []byte) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "pkg.nos")
	if err := os.WriteFile(p, pkg, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}
