package imgbuild

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// Disk layout.
//
//	p1  boot    bootloader configuration and the slot pointer
//
// The slot pointer lives here rather than on the data partition, and that is
// not arbitrary. This filesystem carries no journal, so it can be edited
// offline with debugfs and the change survives. ext4's journal is replayed
// when the kernel mounts it, which silently reverts modifications made outside
// the journal -- an installer writing a boot pointer there appears to succeed
// and the switch boots the old slot anyway.
//
// It is also the right place on its own merits: the pointer is what the
// bootloader reads, it is tiny, and keeping it off the partition holding
// configuration means a corrupted config filesystem cannot make a switch
// unbootable.
//
//	p2  slot A  an image, read-only and immutable
//	p3  slot B  an image
//	p4  data    persistent, shared by both slots
//
// Two slots are what make an upgrade reversible: write to the inactive one,
// point the bootloader at it, and fall back if it does not come up. One slot
// means an upgrade overwrites the only copy.
//
// The data partition is shared rather than per-slot, and that is the decision
// the whole design turns on. Configuration lives there, so a password or a
// routing table set on the box survives an upgrade *and* survives a rollback:
// an upgrade cannot reset it and a rollback cannot lock you out. Per-slot
// state — the writable overlay — lives there too but under a per-slot
// directory, because a component hot-patched onto the running slot must not
// leak into the other slot's known-good state, or rollback stops meaning
// anything.
// Defaults only. The board decides, because flash size is a property of the
// board: see Board.Layout.

// BuildDisk assembles a partitioned disk image containing the composed image
// in slot A and an empty, initialised data partition.
func BuildDisk(o Options, squashfs, kernel, initramfs string) (string, int64, error) {
	bootMiB, slotMiB, dataMiB := o.Board.Layout()
	out := filepath.Join(o.OutDir, "disk.img")
	fmt.Fprintf(o.Log, "==> assembling the disk image\n")

	sq, err := os.Stat(squashfs)
	if err != nil {
		return "", 0, err
	}
	if sq.Size() > int64(slotMiB)*1024*1024 {
		return "", 0, fmt.Errorf("the image is %.1f MiB but a slot is %d MiB — "+
			"raise slot_mib in %s or shrink the profile",
			float64(sq.Size())/(1<<20), slotMiB, o.Board.ID)
	}

	// Slack for GPT itself: the 1 MiB alignment gap at the front and the
	// backup table at the end. Without it the last partition is smaller than
	// its nominal size, which is how a filesystem built to the nominal size
	// ends up written past the end of the disk.
	const gptOverheadMiB = 8
	firstMiB := bootMiB
	if o.Board.PartTable() == "dos" && o.Board.FITMiB > 0 {
		firstMiB = o.Board.FITMiB
	}
	total := int64(firstMiB+2*slotMiB+dataMiB+gptOverheadMiB) * 1024 * 1024
	if err := truncate(out, total); err != nil {
		return "", 0, err
	}

	// GPT, with named partitions. The names are how the initramfs finds the
	// data partition without depending on a device number that changes when
	// hardware does.
	//
	// A board whose firmware cannot read GPT gets a DOS table instead. That is
	// not a preference: U-Boot 2013.01 on the AS5610 has no GPT support
	// compiled in -- its binary carries no EFI or GUID partition strings, only
	// "## Unknown partition table" -- so a GPT install there yields a switch
	// that finds nothing on its own disk. A DOS table has no partition names,
	// so the initramfs falls back to filesystem labels and device order, which
	// it already does.
	//
	// The FIT partition is first and raw. Firmware that reads a filesystem can
	// be given a file; firmware that reads a partition has to be given a
	// partition, and this board's own vendor OS boots exactly this way
	// (usbboot from a raw partition). Using the mechanism already proven on
	// the hardware beats using the tidier one that has never run there.
	// type=uefi on a board whose firmware is the bootloader.
	//
	// Not cosmetic and not a hint: UEFI enumerates EFI system partitions by
	// GUID, and a FAT filesystem in a partition typed `linux` is a FAT
	// filesystem the firmware will not look inside. The symptom is a disk
	// that is laid out perfectly, mounts perfectly under Linux, and offers
	// the firmware nothing to boot.
	bootType := "linux"
	if o.Board.WantsESP() {
		bootType = "uefi"
	}
	script := fmt.Sprintf(`label: gpt
size=%dMiB, type=%s, name="nosaic-boot"
size=%dMiB, type=linux, name="nosaic-slot-a"
size=%dMiB, type=linux, name="nosaic-slot-b"
                type=linux, name="nosaic-data"
`, bootMiB, bootType, slotMiB, slotMiB)
	fitMiB := o.Board.FITMiB
	if o.Board.PartTable() == "dos" {
		if fitMiB > 0 {
			// Four primaries is all a DOS table has, so the FIT partition
			// takes the slot the boot partition would have had and the slot
			// state moves onto the data partition. Both are persistent; the
			// difference is only which filesystem holds them.
			script = fmt.Sprintf(`label: dos
size=%dMiB, type=83
size=%dMiB, type=83
size=%dMiB, type=83
             type=83
`, fitMiB, slotMiB, slotMiB)
		} else {
			script = fmt.Sprintf(`label: dos
size=%dMiB, type=83
size=%dMiB, type=83
size=%dMiB, type=83
             type=83
`, bootMiB, slotMiB, slotMiB)
		}
	}

	cmd := exec.Command("sfdisk", "--quiet", out)
	cmd.Stdin = stringsReader(script)
	if b, err := cmd.CombinedOutput(); err != nil {
		return "", 0, fmt.Errorf("partitioning: %v\n%s", err, b)
	}

	parts, err := partitions(out)
	if err != nil {
		return "", 0, err
	}
	if len(parts) != 4 {
		return "", 0, fmt.Errorf("expected 4 partitions, got %d", len(parts))
	}

	// Left as raw space where it holds the FIT: the installer writes the
	// image into it, because the FIT does not exist yet at this point in the
	// build -- it is made from the kernel and initramfs after the disk is
	// assembled.
	if !(o.Board.PartTable() == "dos" && o.Board.FITMiB > 0) {
		build := buildBootPartition
		if o.Board.WantsESP() {
			build = func(o Options, size int64) (string, error) {
				return buildESP(o, size, kernel, initramfs)
			}
		}
		boot, err := build(o, parts[0].Size*512)
		if err != nil {
			return "", 0, err
		}
		if err := ddInto(boot, out, parts[0].Start*512, parts[0].Size*512, "boot"); err != nil {
			return "", 0, err
		}
	}

	// Slot A gets the image. Slot B is left empty on purpose: a freshly
	// installed switch has nothing to roll back to, and pretending otherwise
	// by duplicating the image would make the first upgrade untestable.
	if err := ddInto(squashfs, out, parts[1].Start*512, parts[1].Size*512, "slot a"); err != nil {
		return "", 0, err
	}

	// Built to the partition that actually exists, rather than to the size it
	// was asked for. GPT overhead makes the last partition smaller than its
	// nominal size, and a filesystem sized by assumption overruns the disk.
	data, err := buildDataPartition(o, parts[3].Size*512)
	if err != nil {
		return "", 0, err
	}
	if err := ddInto(data, out, parts[3].Start*512, parts[3].Size*512, "data"); err != nil {
		return "", 0, err
	}

	fmt.Fprintf(o.Log, "    slot a: %.1f MiB image in a %d MiB slot, slot b: empty, data: %d MiB\n",
		float64(sq.Size())/(1<<20), parts[1].Size*512/(1<<20), parts[3].Size*512/(1<<20))
	return out, parts[0].Start * 512, nil
}

// buildDataPartition makes an ext4 filesystem pre-populated with the directory
// structure the running system expects, so first boot does not have to create
// it and a factory reset is simply "wipe this partition".
func buildDataPartition(o Options, size int64) (string, error) {
	work := filepath.Join(o.Root, ".cache", "image", o.Board.ID, "data")
	if err := os.RemoveAll(work); err != nil {
		return "", err
	}
	for _, d := range []string{
		"config",  // shareable: ports, VLANs, routing. No secrets.
		"secrets", // 0700: password hashes, keys. Never in config.
		"slot-a/upper", "slot-a/work",
		"slot-b/upper", "slot-b/work",
		"log",
	} {
		if err := os.MkdirAll(filepath.Join(work, d), 0o755); err != nil {
			return "", err
		}
	}
	if err := os.Chmod(filepath.Join(work, "secrets"), 0o700); err != nil {
		return "", err
	}
	readme := `This partition is shared by both image slots.

config/   the declarative configuration. Shareable: it is the file to paste
          into a bug report, so it contains no credentials.
secrets/  password hashes, SSH keys. Mode 0700, and deliberately not in
          config/ for exactly that reason.
slot-a/   the writable overlay for slot A. Per-slot, so a change made to the
slot-b/   running slot cannot leak into the other slot's known-good state.

Wiping this partition is a complete factory reset: the switch returns to
"no password, console only".
`
	if err := os.WriteFile(filepath.Join(work, "README"), []byte(readme), 0o644); err != nil {
		return "", err
	}

	img := filepath.Join(o.Root, ".cache", "image", o.Board.ID, "data.ext4")
	if err := truncate(img, size); err != nil {
		return "", err
	}
	// -d populates from a directory, and -U pins the UUID so the filesystem
	// does not get a fresh identity on every build.
	//
	// This partition is not bit-reproducible, and that is fine: it is mutable
	// state by definition, written to on the first boot. What must be
	// reproducible is the image, which is the squashfs, and it is.
	cmd := exec.Command("mke2fs", "-q", "-t", "ext4", "-L", "nosaic-data",
		"-d", work, "-U", "8f3a1c22-0000-4000-8000-6e6f73616963",
		"-F", img)
	if b, err := cmd.CombinedOutput(); err != nil {
		return "", fmt.Errorf("mke2fs: %v\n%s", err, b)
	}
	return img, nil
}

// buildBootPartition makes the small, journal-less filesystem holding the slot
// pointer. ext2 rather than ext4 on purpose: no journal means an offline edit
// is not undone by journal replay the next time the kernel mounts it.
func buildBootPartition(o Options, size int64) (string, error) {
	work := filepath.Join(o.Root, ".cache", "image", o.Board.ID, "bootpart")
	if err := os.RemoveAll(work); err != nil {
		return "", err
	}
	if err := os.MkdirAll(filepath.Join(work, "boot"), 0o755); err != nil {
		return "", err
	}
	// A fresh disk boots slot A, with nothing on trial.
	if err := os.WriteFile(filepath.Join(work, "boot", "active"), []byte("a\n"), 0o644); err != nil {
		return "", err
	}

	img := filepath.Join(o.Root, ".cache", "image", o.Board.ID, "boot.ext2")
	if err := truncate(img, size); err != nil {
		return "", err
	}
	cmd := exec.Command("mke2fs", "-q", "-t", "ext2", "-L", "nosaic-boot",
		"-d", work, "-U", "8f3a1c22-0000-4000-8000-6e6f73616962",
		"-F", img)
	if b, err := cmd.CombinedOutput(); err != nil {
		return "", fmt.Errorf("mke2fs (boot): %v\n%s", err, b)
	}
	return img, nil
}

// buildESP makes the EFI system partition for a board whose firmware is its
// own bootloader.
//
// It replaces buildBootPartition on such a board and does that function's job
// as well as its own: the slot pointer lives here too, because this is still
// the first partition and still the small filesystem the initramfs reads
// before it mounts anything else. FAT has no journal to replay, so the
// property that put the pointer on an ext2 filesystem -- that an offline edit
// stays edited -- holds here for free.
//
// # What goes on it
//
//	/EFI/BOOT/BOOTX64.EFI   the kernel. CONFIG_EFI_STUB makes a bzImage a
//	                        valid PE32+ application, so there is no separate
//	                        bootloader to install and nothing to configure.
//	/EFI/BOOT/initrd.img    the initramfs, loaded by the stub itself.
//	/startup.nsh            the command line, run by the UEFI Shell.
//	/boot/active            the slot pointer.
//
// # Why the command line is in a shell script
//
// The EFI stub takes its command line from the firmware's LoadOptions, and a
// boot entry created with the EDK2 shell's `bcfg boot add` has none -- so a
// kernel launched that way comes up with no console= and no initrd= and says
// nothing about either. The shell, on the other hand, passes arguments, and it
// auto-runs `startup.nsh` from the filesystem it finds one on.
//
// So the arrangement on the first board using this backend is: the firmware's
// boot order reaches the EDK2 shell, the shell runs this script, and the
// script launches the kernel with a full command line. That is unusual and it
// is written down loudly in the board's install page, along with what to do
// when the script is not picked up. The tidier ending -- a real boot entry
// whose optional data carries the command line as UCS-2, written with
// efibootmgr from the running switch -- is in that board's todo.
func buildESP(o Options, size int64, kernel, initramfs string) (string, error) {
	if kernel == "" || initramfs == "" {
		return "", fmt.Errorf("an EFI system partition needs a kernel and an initramfs")
	}
	dir := filepath.Join(o.Root, ".cache", "image", o.Board.ID)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	img := filepath.Join(dir, "esp.vfat")
	if err := truncate(img, size); err != nil {
		return "", err
	}

	// FAT16, not FAT32, and the installer depends on the choice: it verifies
	// the partition by reading the filesystem type string, which FAT16 puts at
	// offset 54 of the first sector and FAT32 puts at 82. UEFI requires
	// firmware to support both, and 16 is what fits a boot partition of this
	// size without mkfs arguing about cluster counts.
	//
	// The label is uppercase because FAT labels are: the filesystem has no
	// lowercase, so asking for one produces a label that does not match what
	// blkid reports, and the initramfs looks the partition up by label.
	cmd := exec.Command("mkfs.vfat", "-F", "16", "-n", "NOSAIC-BOOT", img)
	if b, err := cmd.CombinedOutput(); err != nil {
		return "", fmt.Errorf("mkfs.vfat: %v\n%s", err, b)
	}

	// mtools, rather than mounting. Mounting needs root and a loop device, and
	// the builder container has neither -- the whole image build is deliberately
	// unprivileged, which is why every other filesystem here is made with
	// mke2fs -d.
	run := func(name string, args ...string) error {
		// mtools reads ~/.mtoolsrc and complains about a drive letter it has
		// no configuration for; MTOOLS_SKIP_CHECK stops it refusing an image
		// whose geometry it did not create.
		c := exec.Command(name, args...)
		c.Env = append(os.Environ(), "MTOOLS_SKIP_CHECK=1")
		if b, err := c.CombinedOutput(); err != nil {
			return fmt.Errorf("%s: %v\n%s", name, err, b)
		}
		return nil
	}
	for _, d := range []string{"::/EFI", "::/EFI/BOOT", "::/boot"} {
		if err := run("mmd", "-i", img, d); err != nil {
			return "", err
		}
	}

	// ⚠ BOOTX64.EFI IS THE NAME, AND IT IS NOT A CONVENTION WE CHOSE.
	//
	// UEFI's removable-media path is \EFI\BOOT\BOOTX64.EFI exactly, and it is
	// the one path firmware will boot without a boot variable telling it to.
	// A kernel installed under any other name needs a boot entry created
	// before the box can ever start it, which on a switch means a console
	// session in the EFI shell before the first boot rather than after a
	// failed one.
	for _, f := range [][2]string{
		{kernel, "::/EFI/BOOT/BOOTX64.EFI"},
		{initramfs, "::/EFI/BOOT/initrd.img"},
	} {
		if err := run("mcopy", "-i", img, "-o", f[0], f[1]); err != nil {
			return "", err
		}
	}

	nsh := startupScript(o)

	// ⚠ ITERATED OVER FILESYSTEMS RATHER THAN HARDCODING fs0.
	//
	// Which alias the shell gives our partition depends on what else is
	// plugged in: insert a USB stick to copy an image over and the internal
	// disk can become fs1. A script that says fs0 then launches a kernel off
	// the USB stick, or nothing at all, and the failure is a shell prompt in
	// a rack.
	stage := filepath.Join(dir, "startup.nsh")
	if err := os.WriteFile(stage, []byte(nsh), 0o644); err != nil {
		return "", err
	}
	if err := run("mcopy", "-i", img, "-o", stage, "::/startup.nsh"); err != nil {
		return "", err
	}

	// A fresh disk boots slot A, with nothing on trial. Same file and same
	// contents as buildBootPartition writes; the filesystem under it differs
	// and nothing above it knows.
	active := filepath.Join(dir, "active")
	if err := os.WriteFile(active, []byte("a\n"), 0o644); err != nil {
		return "", err
	}
	if err := run("mcopy", "-i", img, "-o", active, "::/boot/active"); err != nil {
		return "", err
	}

	fmt.Fprintf(o.Log, "    EFI system partition: %d MiB, kernel as \\EFI\\BOOT\\BOOTX64.EFI\n",
		size/(1<<20))
	return img, nil
}

// startupScript is the command line the kernel is launched with, as a UEFI
// Shell script.
//
// Split out of buildESP so it can be read and tested without mtools: the
// filesystem this lands on needs three external tools to inspect, and the
// thing most likely to be wrong is this text.
func startupScript(o Options) string {
	consoleDev, consoleBaud := o.Board.ConsolePort()
	return fmt.Sprintf(`# NOSaic %s for %s. Generated -- do not edit.
#
# The UEFI Shell runs this. It exists because the EFI stub reads its command
# line from the firmware and a boot entry does not carry one.
@echo -off
echo NOSaic %s
echo .
for %%v in fs0 fs1 fs2 fs3
    if exist %%v:\EFI\BOOT\BOOTX64.EFI then
        %%v:
        echo booting from %%v:
        \EFI\BOOT\BOOTX64.EFI initrd=\EFI\BOOT\initrd.img console=%s,%dn8 %s
    endif
endfor
echo NOSaic: no EFI system partition found on fs0..fs3
`, o.Version, o.Board.ID, o.Version, consoleDev, consoleBaud, o.Board.KernelParams)
}

type partition struct {
	Start int64 `json:"start"`
	Size  int64 `json:"size"`
	Name  string
}

func partitions(disk string) ([]partition, error) {
	out, err := exec.Command("sfdisk", "--json", disk).Output()
	if err != nil {
		return nil, err
	}
	var doc struct {
		PartitionTable struct {
			Partitions []partition `json:"partitions"`
		} `json:"partitiontable"`
	}
	if err := json.Unmarshal(out, &doc); err != nil {
		return nil, err
	}
	return doc.PartitionTable.Partitions, nil
}

func truncate(path string, size int64) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return f.Truncate(size)
}

// ddInto writes src into dst at offset, refusing to exceed limit.
//
// Writing past a partition's end does not fail loudly: it silently corrupts
// whatever comes after, which on a GPT disk is the backup partition table. The
// symptom is a disk that mostly works and a filesystem that will not mount.
func ddInto(src, dst string, offset, limit int64, what string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()

	fi, err := in.Stat()
	if err != nil {
		return err
	}
	if fi.Size() > limit {
		return fmt.Errorf("%s: %.1f MiB does not fit in a %.1f MiB partition",
			what, float64(fi.Size())/(1<<20), float64(limit)/(1<<20))
	}
	out, err := os.OpenFile(dst, os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	defer out.Close()
	if _, err := out.Seek(offset, 0); err != nil {
		return err
	}
	_, err = io.Copy(out, in)
	return err
}

func stringsReader(s string) io.Reader { return strings.NewReader(s) }
