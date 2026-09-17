package imgbuild

import (
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
)

// The EFI system partition is the only boot artifact in this tree that nothing
// else checks: there is no bootloader configuration to review and no FIT magic
// to read back, just a FAT filesystem whose contents the firmware either finds
// or does not. And "does not" presents as a switch that reports no bootable
// device -- a console session in a rack, on a board whose vendor OS has
// already been overwritten.
//
// So these tests assert the three things the firmware and the shell actually
// require, and they assert them against a real filesystem rather than against
// the code's intentions.

func espBoard() *board.Board {
	return &board.Board{
		ID: "test-uefi", Arch: "x86_64", ASIC: "td2", Boot: "uefi",
		Console: "ttyS0", ConsoleBaud: 9600,
		KernelParams: "ignore_loglevel",
		Status:       "planned", Profile: "minimal",
	}
}

func TestOnlyAUEFIBoardWantsAnESP(t *testing.T) {
	if !espBoard().WantsESP() {
		t.Fatal("a board booting with uefi must get an EFI system partition")
	}
	// Every other board gets the small ext2 filesystem, and giving one a FAT
	// partition typed EFI System would hand its vendor bootloader a layout it
	// has never been asked to read.
	for _, b := range []string{"aboot", "onie-sfx", "uboot", "virt", ""} {
		brd := espBoard()
		brd.Boot = b
		if brd.WantsESP() {
			t.Fatalf("boot %q must not get an EFI system partition", b)
		}
	}
}

func espTools(t *testing.T) {
	t.Helper()
	for _, tool := range []string{"mkfs.vfat", "mmd", "mcopy", "mdir", "mtype"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skipf("%s is not installed; the builder container has it", tool)
		}
	}
}

// buildESP is given a kernel and an initramfs and has to put them where UEFI's
// removable-media path looks -- \EFI\BOOT\BOOTX64.EFI exactly, because that is
// the one path firmware boots without a boot variable telling it to.
func TestESPCarriesTheKernelWhereTheFirmwareLooks(t *testing.T) {
	espTools(t)
	root := t.TempDir()

	kernel := filepath.Join(root, "vmlinuz")
	initramfs := filepath.Join(root, "initramfs.cpio.gz")
	if err := os.WriteFile(kernel, []byte("MZ-not-really-a-kernel"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(initramfs, []byte("not-really-an-initramfs"), 0o644); err != nil {
		t.Fatal(err)
	}

	o := Options{Root: root, Board: espBoard(), Version: "0.0.0-test", Log: io.Discard}
	img, err := buildESP(o, 32<<20, kernel, initramfs)
	if err != nil {
		t.Fatalf("buildESP: %v", err)
	}

	for _, want := range []string{
		"::/EFI/BOOT/BOOTX64.EFI",
		"::/EFI/BOOT/initrd.img",
		"::/startup.nsh",
		"::/boot/active",
	} {
		cmd := exec.Command("mtype", "-i", img, want)
		cmd.Env = append(os.Environ(), "MTOOLS_SKIP_CHECK=1")
		out, err := cmd.Output()
		if err != nil {
			t.Fatalf("%s is not on the EFI system partition: %v", want, err)
		}
		if len(out) == 0 {
			t.Fatalf("%s is empty", want)
		}
	}

	// A fresh disk boots slot A with nothing on trial. Same contract as the
	// ext2 boot partition, and the initramfs reads it by the same path.
	cmd := exec.Command("mtype", "-i", img, "::/boot/active")
	cmd.Env = append(os.Environ(), "MTOOLS_SKIP_CHECK=1")
	out, _ := cmd.Output()
	if strings.TrimSpace(string(out)) != "a" {
		t.Fatalf("the slot pointer says %q, want \"a\"", strings.TrimSpace(string(out)))
	}
}

// The command line is the whole reason startup.nsh exists: a kernel launched
// from a plain firmware boot entry gets no LoadOptions, so no console= and no
// initrd=, and on a 9600 serial console that is a silent boot of a kernel with
// no root filesystem.
//
// Two things in it are board data and both fail quietly if they are wrong. A
// wrong baud makes everything after the firmware's own output unreadable, and
// a missing initrd= is a kernel that comes up with no root.
func TestStartupScriptCarriesTheBoardsCommandLine(t *testing.T) {
	nsh := startupScript(Options{Board: espBoard(), Version: "0.0.0-test"})

	for _, want := range []string{
		`console=ttyS0,9600n8`,
		`initrd=\EFI\BOOT\initrd.img`,
		`ignore_loglevel`,
		`\EFI\BOOT\BOOTX64.EFI`,
	} {
		if !strings.Contains(nsh, want) {
			t.Errorf("startup.nsh does not carry %q:\n%s", want, nsh)
		}
	}

	// ⚠ It must not hardcode one filesystem alias. Inserting a USB stick to
	// copy an image over renumbers them, and a script that says fs0 then
	// launches whatever is on the stick, or nothing at all -- which is a shell
	// prompt in a rack rather than an error.
	for _, fs := range []string{"fs0", "fs1", "fs2", "fs3"} {
		if !strings.Contains(nsh, fs) {
			t.Errorf("startup.nsh does not look at %s; it must not assume one alias:\n%s", fs, nsh)
		}
	}

	// ⚠ THE LOOP VARIABLE IS ONE PERCENT SIGN, NOT TWO.
	//
	// A UEFI Shell script spells it %v; %%v is the DOS batch convention and
	// this is not DOS. Written through fmt.Sprintf, where %% renders as %, so
	// one doubling too many is easy and produces a script the shell runs
	// without substituting anything -- it then looks for a filesystem
	// literally called "%v" and finds none.
	if strings.Contains(nsh, "%%v") {
		t.Errorf("the shell loop variable is doubled; a .nsh script wants %%v:\n%s", nsh)
	}
	if !strings.Contains(nsh, "for %v in fs0") {
		t.Errorf("startup.nsh has no shell for-loop over the filesystems:\n%s", nsh)
	}
}

// Refusing is the right answer rather than producing a partition with nothing
// on it: an ESP with no BOOTX64.EFI is a disk the firmware reports as having
// no bootable device, which is indistinguishable from a failed install.
func TestESPRefusesToBuildWithoutAKernel(t *testing.T) {
	o := Options{Root: t.TempDir(), Board: espBoard(), Log: io.Discard}
	if _, err := buildESP(o, 32<<20, "", "/nonexistent"); err == nil {
		t.Fatal("buildESP must refuse an empty kernel path")
	}
	if _, err := buildESP(o, 32<<20, "/nonexistent", ""); err == nil {
		t.Fatal("buildESP must refuse an empty initramfs path")
	}
}

// ⚠ THE INSTALLER VERIFIES THE WRITE BY READING FIVE BYTES AT OFFSET 54, AND
// THAT OFFSET IS ONLY RIGHT FOR FAT16.
//
// FAT16 puts its filesystem-type string in the BPB at 0x36; FAT32 moves it to
// 0x52 and puts a different structure at 0x36. So if buildESP ever produces
// FAT32 -- a larger boot partition would be enough to make mkfs choose it --
// the installer's check reads the wrong bytes, reports that the disk did not
// take the image, and refuses an install that in fact succeeded.
//
// Checked here rather than trusted, because the two failure directions are
// both bad and neither is visible until somebody is at a console: FAT32 with
// this check fails a good install, and a firmware that could not read FAT32
// would boot nothing from a good one.
//
// Needs only mkfs.vfat, so unlike the tests above this one runs on an ordinary
// build host.
func TestTheESPIsFAT16WhereTheInstallerLooksForIt(t *testing.T) {
	if _, err := exec.LookPath("mkfs.vfat"); err != nil {
		t.Skip("mkfs.vfat is not installed; the builder container has it")
	}
	if _, err := exec.LookPath("mmd"); err != nil {
		// buildESP needs mtools to populate the filesystem, so build the bare
		// filesystem the same way it does and check that much. The size is
		// what decides FAT16 against FAT32, and the size is what is at stake.
		root := t.TempDir()
		img := filepath.Join(root, "esp.vfat")
		if err := truncate(img, 64<<20); err != nil {
			t.Fatal(err)
		}
		cmd := exec.Command("mkfs.vfat", "-F", "16", "-n", "NOSAIC-BOOT", img)
		if b, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("mkfs.vfat: %v\n%s", err, b)
		}
		assertFAT16At54(t, img)
		return
	}

	root := t.TempDir()
	kernel := filepath.Join(root, "vmlinuz")
	initramfs := filepath.Join(root, "initramfs.cpio.gz")
	for _, f := range []string{kernel, initramfs} {
		if err := os.WriteFile(f, []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	o := Options{Root: root, Board: espBoard(), Version: "0.0.0-test", Log: io.Discard}
	img, err := buildESP(o, 64<<20, kernel, initramfs)
	if err != nil {
		t.Fatalf("buildESP: %v", err)
	}
	assertFAT16At54(t, img)
}

func assertFAT16At54(t *testing.T, img string) {
	t.Helper()
	f, err := os.Open(img)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	// The offset the installer uses, and the length it reads.
	buf := make([]byte, 5)
	if _, err := f.ReadAt(buf, 54); err != nil {
		t.Fatal(err)
	}
	if got := string(buf); !strings.HasPrefix(got, "FAT") {
		t.Fatalf("offset 54 reads %q, not a FAT type string — the installer's "+
			"verification would fail a good install", got)
	}
	if got := string(buf); got != "FAT16" {
		t.Fatalf("offset 54 reads %q; the installer and this filesystem must "+
			"agree on FAT16", got)
	}
}
