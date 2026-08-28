package boot

import (
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
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
	for _, id := range []string{"onie-sfx", "aboot", "virt", "uboot"} {
		b, _ := For(id)
		if _, err := b.Wrap(Image{Board: "b", Version: "1"}, dir, io.Discard); err == nil {
			t.Errorf("%s accepted an image with no artifacts", id)
		}
	}
}

// mkimageOrSkip keeps the U-Boot tests honest on a machine without u-boot-tools
// rather than failing for a reason that has nothing to do with the code.
func mkimageOrSkip(t *testing.T) {
	t.Helper()
	// Skip only when u-boot-tools is absent entirely. A machine that has
	// mkimage but not dtc is a broken build environment, and skipping there
	// would hide exactly the gap that made CI red.
	if _, err := exec.LookPath("mkimage"); err != nil {
		t.Skip("mkimage not installed; it is in builder/Dockerfile.build")
	}
}

func TestUBootRefusesWithoutBoardAddresses(t *testing.T) {
	mkimageOrSkip(t)
	img, dir := fixture(t)
	b, _ := For("uboot")

	// Guessing a load address is how a board ends up hanging with nothing on
	// the console, so refusing is the whole point of this test.
	for _, missing := range []struct {
		what string
		img  Image
	}{
		{"arch", Image{Kernel: img.Kernel, Initramfs: img.Initramfs,
			UBootLoad: "0x1000000", UBootEntry: "0x1000000"}},
		{"load address", Image{Kernel: img.Kernel, Initramfs: img.Initramfs,
			UBootArch: "ppc", UBootEntry: "0x1000000"}},
		{"entry address", Image{Kernel: img.Kernel, Initramfs: img.Initramfs,
			UBootArch: "ppc", UBootLoad: "0x1000000"}},
	} {
		if _, err := b.Wrap(missing.img, dir, io.Discard); err == nil {
			t.Errorf("uboot built an image with no %s", missing.what)
		}
	}
}

func TestUBootFITIsReadableByMkimage(t *testing.T) {
	mkimageOrSkip(t)
	img, dir := fixture(t)
	img.UBootArch, img.UBootLoad, img.UBootEntry = "ppc", "0x01000000", "0x01000000"

	b, _ := For("uboot")
	out, err := b.Wrap(img, dir, io.Discard)
	if err != nil {
		t.Fatal(err)
	}

	// Asking mkimage to read the FIT back proves it is a FIT rather than
	// merely a file that got written -- and that the kernel and the initramfs
	// both landed in it, which a length check would not show.
	listed, err := exec.Command("mkimage", "-l", out).CombinedOutput()
	if err != nil {
		t.Fatalf("mkimage could not read back its own output: %v\n%s", err, listed)
	}
	for _, want := range []string{"kernel", "ramdisk", "01000000"} {
		if !strings.Contains(string(listed), want) {
			t.Errorf("FIT has no %q:\n%s", want, listed)
		}
	}
}

func TestUBootCommandsMatchTheImage(t *testing.T) {
	mkimageOrSkip(t)
	img, dir := fixture(t)
	img.UBootArch, img.UBootLoad, img.UBootEntry = "arm", "0x02000000", "0x02000000"

	b, _ := For("uboot")
	out, err := b.Wrap(img, dir, io.Discard)
	if err != nil {
		t.Fatal(err)
	}

	// The commands are typed at a console by a person, so a filename or an
	// address that disagrees with the image is a bricked bring-up session.
	notes, err := os.ReadFile(filepath.Join(dir, "uboot-commands.txt"))
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{filepath.Base(out), img.UBootLoad} {
		if !strings.Contains(string(notes), want) {
			t.Errorf("uboot-commands.txt does not mention %q:\n%s", want, notes)
		}
	}
}

// Aboot refuses a SWI with no signature unless the version member says
// BLESSED=1. NOSaic does not sign, so without this the image is rejected by
// the bootloader before any of our code runs -- and the only symptom is a
// switch that stays on EOS.
//
// The field set here was read off two EOS SWIs pulled from a switch's flash.
func TestAbootSWIIsBlessedAndOrderedLikeTheVendors(t *testing.T) {
	img, dir := fixture(t)
	b, _ := For("aboot")
	out, err := b.Wrap(img, dir, io.Discard)
	if err != nil {
		t.Fatal(err)
	}

	version, err := exec.Command("unzip", "-p", out, "version").Output()
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(version), "BLESSED=1") {
		t.Errorf("version has no BLESSED=1, so Aboot will refuse it:\n%s", version)
	}
	if !strings.Contains(string(version), "SWI_MAX_HWEPOCH=") {
		t.Errorf("version has no SWI_MAX_HWEPOCH:\n%s", version)
	}
	// EOS omits SWI_ARCH, which is what proves Aboot does not need it. Adding
	// one we invented would be guessing at a field the bootloader parses.
	if strings.Contains(string(version), "SWI_ARCH") {
		t.Errorf("SWI_ARCH is set, but neither vendor SWI sets it:\n%s", version)
	}

	listing, err := exec.Command("unzip", "-l", out).Output()
	if err != nil {
		t.Fatal(err)
	}
	iv := strings.Index(string(listing), "version")
	ib := strings.Index(string(listing), "boot0")
	if iv < 0 || ib < 0 || iv > ib {
		t.Errorf("version should be the first member, as in the vendor's SWIs:\n%s", listing)
	}
}

// The board's epoch is board data, and an image claiming an epoch lower than
// the switch is refused by Aboot.
func TestAbootHWEpochComesFromTheBoard(t *testing.T) {
	img, dir := fixture(t)
	img.AbootMaxHWEpoch = "3"
	b, _ := For("aboot")
	out, err := b.Wrap(img, dir, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	version, _ := exec.Command("unzip", "-p", out, "version").Output()
	if !strings.Contains(string(version), "SWI_MAX_HWEPOCH=3") {
		t.Errorf("board epoch not carried into the SWI:\n%s", version)
	}
}

// Aboot EXPORTS swipath; it does not pass it as an argument. This ran as
// `boot0 <path>` originally, which finds nothing on a real switch -- and the
// failure is a kexec of a file that is not there, on hardware, with a console
// message that does not say why.
func TestAbootBoot0ReadsSwipathFromTheEnvironment(t *testing.T) {
	img, dir := fixture(t)
	b, _ := For("aboot")
	if _, err := b.Wrap(img, dir, io.Discard); err != nil {
		t.Fatal(err)
	}

	// Unpacked-directory form, so boot0 takes the cp branch and needs no unzip.
	swidir := t.TempDir()
	for name, content := range map[string]string{
		"nosaic-kernel": "KERNEL-CONTENT",
		"nosaic-initrd": "INITRAMFS-CONTENT",
	} {
		if err := os.WriteFile(filepath.Join(swidir, name), []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	boot0, err := exec.Command("unzip", "-p",
		filepath.Join(dir, "NOSaic-"+img.Version+"-"+img.Board+".swi"), "boot0").Output()
	if err != nil {
		t.Fatal(err)
	}
	b0 := filepath.Join(swidir, "boot0")
	if err := os.WriteFile(b0, boot0, 0o755); err != nil {
		t.Fatal(err)
	}

	// Stub kexec so nothing is actually loaded, and record how it was called.
	binDir := t.TempDir()
	record := filepath.Join(binDir, "kexec.log")
	stub := "#!/bin/sh\necho \"$@\" >> " + record + "\n"
	if err := os.WriteFile(filepath.Join(binDir, "kexec"), []byte(stub), 0o755); err != nil {
		t.Fatal(err)
	}

	cmd := exec.Command("sh", b0)
	cmd.Env = append(os.Environ(),
		"swipath="+swidir,
		"PATH="+binDir+":"+os.Getenv("PATH"))
	if outp, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("boot0 failed with swipath in the environment: %v\n%s", err, outp)
	}

	got, err := os.ReadFile(record)
	if err != nil {
		t.Fatalf("boot0 never reached kexec: %v", err)
	}
	if !strings.Contains(string(got), "/tmp/nosaic-kernel") {
		t.Errorf("kexec was not given our kernel, got: %s", got)
	}
}

// Aboot's dry run only works if the SWI cooperates: it exports testonly and
// expects boot0 to stage the kernel and then stop. A boot0 that ignores it
// boots for real when somebody asked for a rehearsal.
func TestAbootBoot0HonoursTestonly(t *testing.T) {
	img, dir := fixture(t)
	b, _ := For("aboot")
	out, err := b.Wrap(img, dir, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	boot0, err := exec.Command("unzip", "-p", out, "boot0").Output()
	if err != nil {
		t.Fatal(err)
	}
	script := string(boot0)

	if !strings.Contains(script, "${testonly}") {
		t.Error("boot0 never looks at testonly, so a dry run would boot the switch")
	}
	// It has to come after staging and before the jump, or it proves nothing
	// and stops nothing respectively.
	load := strings.Index(script, "kexec --load")
	check := strings.Index(script, "${testonly}")
	exec := strings.LastIndex(script, "kexec --exec")
	if load < 0 || check < 0 || exec < 0 || !(load < check && check < exec) {
		t.Errorf("the testonly check must sit between kexec --load and kexec --exec (load=%d check=%d exec=%d)",
			load, check, exec)
	}
}

// Aboot parses SWI_VERSION as EOS's series.major.minor and refuses anything
// below 4.14.7 before running any of the image. A NOSaic version in that field
// reads as 0.0.0 and the switch rejects the SWI outright, which is how the
// first real boot attempt on the 7050SX2 ended.
func TestAbootSWIVersionSatisfiesTheBootloaderFloor(t *testing.T) {
	img, dir := fixture(t)
	b, _ := For("aboot")
	out, err := b.Wrap(img, dir, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	version, err := exec.Command("unzip", "-p", out, "version").Output()
	if err != nil {
		t.Fatal(err)
	}

	var swi string
	for _, line := range strings.Split(string(version), "\n") {
		if strings.HasPrefix(line, "SWI_VERSION=") {
			swi = strings.TrimPrefix(line, "SWI_VERSION=")
		}
	}
	if swi == "" {
		t.Fatal("no SWI_VERSION")
	}
	parts := strings.Split(swi, ".")
	if len(parts) < 3 {
		t.Fatalf("SWI_VERSION %q is not series.major.minor, which is how Aboot reads it", swi)
	}
	series, err := strconv.Atoi(parts[0])
	if err != nil || series < 4 {
		t.Errorf("SWI_VERSION %q: Aboot refuses a series below 4", swi)
	}

	// And ours must still be recoverable, or the image cannot say what it is.
	if !strings.Contains(string(version), "NOSAIC_VERSION="+img.Version) {
		t.Errorf("the image does not carry its own version:\n%s", version)
	}
}

// The board's kernel arguments have to reach the kernel. boot0 extracts them
// from the SWI, because they live inside it -- testing for the file beside the
// archive is always false, and the first dry run on hardware booted with the
// console setting alone and no memmap reservation.
func TestAbootBoot0ExtractsAndUsesKernelParams(t *testing.T) {
	img, dir := fixture(t)
	img.KernelParams = "memmap=64M$0xd0000000 iomem=relaxed"
	b, _ := For("aboot")
	out, err := b.Wrap(img, dir, io.Discard)
	if err != nil {
		t.Fatal(err)
	}

	if listing, err := exec.Command("unzip", "-l", out).Output(); err != nil {
		t.Fatal(err)
	} else if !strings.Contains(string(listing), "kernel-params") {
		t.Fatalf("the SWI does not carry kernel-params:\n%s", listing)
	}

	boot0, err := exec.Command("unzip", "-p", out, "boot0").Output()
	if err != nil {
		t.Fatal(err)
	}
	script := string(boot0)
	if !strings.Contains(script, "unzip -oq \"$swipath\" kernel-params") {
		t.Error("boot0 never extracts kernel-params from the SWI")
	}
	// The command line must be built from the extracted copy. Reading it
	// through $swipath is the original bug: after extraction $swipath is
	// still the archive, so the test is false and the parameters vanish.
	// (Inside the unpacked-directory branch, $swipath/kernel-params is
	// legitimate -- there it really is a directory.)
	if strings.Contains(script, `CMDLINE="$CMDLINE $(cat "$swipath/kernel-params")"`) {
		t.Error("boot0 builds the command line from $swipath, which is the archive")
	}
	if !strings.Contains(script, "cat /tmp/kernel-params") {
		t.Error("boot0 does not read the extracted kernel-params")
	}
}

// The vendor's boot0 cycles the management interface around the kexec, for two
// documented reasons: bringing it up copies the MAC from the SCD mailbox into
// the NIC register, where the next kernel's tg3 looks for it, and bringing it
// down stops the NIC DMA-ing into memory the next kernel has not initialised.
//
// Our first boot on the 7050SX2 did neither: tg3 aborted with "Could not
// obtain valid ethernet address", and the kernel hung shortly after handover.
func TestAbootBoot0CyclesTheManagementInterface(t *testing.T) {
	img, dir := fixture(t)
	b, _ := For("aboot")
	out, err := b.Wrap(img, dir, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	boot0, err := exec.Command("unzip", "-p", out, "boot0").Output()
	if err != nil {
		t.Fatal(err)
	}
	script := string(boot0)

	up := strings.Index(script, `ip link set "$NETDEV" up`)
	down := strings.Index(script, `ip link set "$NETDEV" down`)
	load := strings.Index(script, "kexec --load")
	if up < 0 || down < 0 {
		t.Fatal("boot0 does not cycle the management interface; tg3 will find no MAC")
	}
	if !(up < down && down < load) {
		t.Errorf("the interface must go up, then down, then kexec (up=%d down=%d load=%d)", up, down, load)
	}
}
