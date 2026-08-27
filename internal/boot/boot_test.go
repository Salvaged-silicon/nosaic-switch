package boot

import (
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// fixture builds a plausible set of image artifacts with recognisable content,
// so a test can tell whether the right bytes came back out.
func fixture(t *testing.T) (Image, string) {
	t.Helper()
	dir := t.TempDir()
	write := func(name, content string) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	return Image{
		Kernel:    write("vmlinuz", "KERNEL-CONTENT"),
		Initramfs: write("initramfs.cpio.gz", "INITRAMFS-CONTENT"),
		Squashfs:  write("rootfs.sqsh", "SQUASHFS-CONTENT"),
		Disk:      write("disk.img", "DISK-IMAGE-CONTENT"),
		Board:     "test-board",
		Arch:      "x86_64",
		Version:   "9.9.9",
	}, dir
}

func TestEveryBackendIsRegisteredAndDescribed(t *testing.T) {
	if len(All()) == 0 {
		t.Fatal("no bootloaders registered")
	}
	for _, id := range All() {
		b, err := For(id)
		if err != nil {
			t.Fatalf("For(%q): %v", id, err)
		}
		// Describe is what the generated per-switch pages print. An empty one
		// means a board's installation instructions would be blank.
		if b.Describe() == "" {
			t.Errorf("%s has no description", id)
		}
	}
}

func TestUnknownBootloaderIsAnError(t *testing.T) {
	if _, err := For("grub-legacy"); err == nil {
		t.Fatal("an unknown bootloader should be an error, not a silent default")
	}
}

// The ONIE installer is a shell script with a tar appended. This checks the
// mechanism actually works rather than that the file merely exists: the offset
// the script computes for itself must land exactly on the payload.
func TestONIEInstallerExtractsItsOwnPayload(t *testing.T) {
	img, dir := fixture(t)
	b, _ := For("onie-sfx")
	out, err := b.Wrap(img, dir, io.Discard)
	if err != nil {
		t.Fatalf("wrap: %v", err)
	}

	if !strings.HasSuffix(out, ".bin") {
		t.Errorf("ONIE installers are .bin files, got %s", out)
	}
	head, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(string(head), "#!/bin/sh") {
		t.Fatal("the installer must be executable as a script: ONIE runs it directly")
	}

	// Exactly what the installer does to itself, run here instead of on a
	// switch: find the marker, skip past it, and read the disk image out.
	script := `set -e
SKIP=$(awk '/^__NOSAIC_PAYLOAD__$/ { print NR + 1; exit 0; }' "$1")
tail -n +$SKIP "$1" | tar -xO disk.img`
	cmd := exec.Command("sh", "-c", script, "sh", out)
	got, err := cmd.Output()
	if err != nil {
		t.Fatalf("the installer could not extract its own payload: %v", err)
	}
	if string(got) != "DISK-IMAGE-CONTENT" {
		t.Fatalf("payload came back as %q, want the disk image", string(got))
	}
}

func TestONIEInstallerIsExecutable(t *testing.T) {
	img, dir := fixture(t)
	b, _ := For("onie-sfx")
	out, _ := b.Wrap(img, dir, io.Discard)
	fi, err := os.Stat(out)
	if err != nil {
		t.Fatal(err)
	}
	if fi.Mode().Perm()&0o111 == 0 {
		t.Error("ONIE executes the installer, so it has to be executable")
	}
}

// The SWI shape was read off a real switch: a zip, stored rather than
// deflated, containing a boot0 that Aboot runs.
func TestAbootSWIShape(t *testing.T) {
	img, dir := fixture(t)
	b, _ := For("aboot")
	out, err := b.Wrap(img, dir, io.Discard)
	if err != nil {
		t.Fatalf("wrap: %v", err)
	}
	if !strings.HasSuffix(out, ".swi") {
		t.Errorf("Aboot boots .swi files, got %s", out)
	}

	listing, err := exec.Command("unzip", "-l", out).Output()
	if err != nil {
		t.Fatalf("the SWI is not a readable zip: %v", err)
	}
	for _, want := range []string{"boot0", "version", "nosaic-kernel", "nosaic-initrd"} {
		if !strings.Contains(string(listing), want) {
			t.Errorf("the SWI has no %s:\n%s", want, listing)
		}
	}

	// Stored, not deflated. The vendor's own SWI is stored, and Aboot has to
	// read boot0 out of this before anything of ours runs.
	verbose, err := exec.Command("unzip", "-v", out).Output()
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(verbose), "Stored") {
		t.Errorf("SWI members should be stored, not compressed:\n%s", verbose)
	}

	// The kernel must come back byte-for-byte: a SWI whose kernel is subtly
	// altered would fail at kexec, on hardware, with nothing useful on the
	// console.
	kernel, err := exec.Command("unzip", "-p", out, "nosaic-kernel").Output()
	if err != nil {
		t.Fatal(err)
	}
	if string(kernel) != "KERNEL-CONTENT" {
		t.Errorf("kernel came back as %q", string(kernel))
	}
}

// boot-config is what actually points Aboot at the image, and it lives in
// flash rather than in the SWI. Producing the SWI without it would give
// somebody an image and no way to boot it.
func TestAbootWritesBootConfig(t *testing.T) {
	img, dir := fixture(t)
	b, _ := For("aboot")
	out, err := b.Wrap(img, dir, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	bc, err := os.ReadFile(filepath.Join(dir, "boot-config"))
	if err != nil {
		t.Fatalf("no boot-config written: %v", err)
	}
	want := "SWI=flash:/" + filepath.Base(out)
	if strings.TrimSpace(string(bc)) != want {
		t.Errorf("boot-config is %q, want %q", strings.TrimSpace(string(bc)), want)
	}
}

func TestBackendsRefuseIncompleteImages(t *testing.T) {
	dir := t.TempDir()
	for _, id := range []string{"onie-sfx", "aboot", "virt"} {
		b, _ := For(id)
		if _, err := b.Wrap(Image{Board: "b", Version: "1"}, dir, io.Discard); err == nil {
			t.Errorf("%s accepted an image with no artifacts", id)
		}
	}
}
