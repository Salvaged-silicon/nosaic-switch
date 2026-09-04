package upgrade

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// A file-backed layout keeps its slots as files on the bootloader's own
// filesystem, for a board where there is no room for partitions of ours.
//
// The 7050SX2 is the case that forced it: its eMMC is allocated entirely to
// Aboot's FAT, which also holds the vendor images that are the box's way back,
// so there is nowhere to put a slot without cutting into them. The initramfs
// loop-mounts the slot file instead, and everything above that -- trial boots,
// the tries counter, rollback -- is the same code as on a partitioned board.
//
// This is also the vendor's own approach rather than a workaround: EOS
// loop-mounts its root filesystem out of a file on this same FAT.

// slotFileName is what the initramfs looks for. slotdev() resolves slot "a" to
// "$FLASH/nosaic-slot-a.sqsh", so these two must not drift apart.
func slotFileName(slot string) string { return "nosaic-slot-" + slot + ".sqsh" }

// fileBacked reports whether state and slots are plain files on a mounted
// filesystem rather than offsets in a partition table.
//
// A directory means the slots are files inside it; anything else is a block
// device or a disk image with a partition table.
func (d Disk) fileBacked() bool {
	if d.Files {
		return true
	}
	fi, err := os.Stat(d.Path)
	return err == nil && fi.IsDir()
}

// StateDir is where the running system keeps its boot pointer.
//
// Resolved the way the initramfs resolves it: a board with a boot partition
// keeps the pointer there, and one whose bootloader owns the whole disk has no
// boot partition, so it goes on the data filesystem instead. Returns "" if
// neither is mounted.
func StateDir() string {
	for _, dir := range []string{"/mnt/boot/boot", "/mnt/data/boot"} {
		if fi, err := os.Stat(dir); err == nil && fi.IsDir() {
			return dir
		}
	}
	return ""
}

// Local is the running switch's own boot pointer, for the commands that only
// read or set it.
//
// Deliberately not addressed through the bootloader's filesystem. Confirming a
// trial never writes a slot, so requiring the flash to be mounted would make
// the confirmation depend on something it does not use -- which is exactly
// what happened: the trial-confirm service was handed /mnt/flash, that path
// did not exist in the running root, and a healthy image quietly failed to
// commit.
func Local() (Disk, error) {
	dir := StateDir()
	if dir == "" {
		return Disk{}, fmt.Errorf("this system has no mounted boot state: " +
			"neither /mnt/boot/boot nor /mnt/data/boot is there")
	}
	parent := filepath.Dir(dir)
	return Disk{Path: parent, Data: parent, Files: true}, nil
}

// dataDir is the mounted persistent filesystem: boot state and the per-slot
// overlays live here.
func (d Disk) dataDir() string {
	if d.Data != "" {
		return d.Data
	}
	return "/mnt/data"
}

func (d Disk) logf(format string, a ...any) {
	if d.Log != nil {
		fmt.Fprintf(d.Log, format, a...)
	}
}

func readStateDir(dir string) (map[string]string, error) {
	out := map[string]string{}
	for _, name := range []string{"active", "trial", "tries"} {
		// Absent is a legitimate state, not an error: a switch that has never
		// been upgraded has no trial recorded.
		b, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			continue
		}
		out[name] = string(b)
	}
	return out, nil
}

func writeStateDir(dir string, files map[string]string) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	for name, content := range files {
		p := filepath.Join(dir, name)
		if content == "" {
			// Clearing a trial is expressed as an empty value. Removing the
			// file is what the initramfs reads as "no trial"; writing an
			// empty one would leave a trial that never expires.
			if err := os.Remove(p); err != nil && !os.IsNotExist(err) {
				return err
			}
			continue
		}
		if err := os.WriteFile(p, []byte(content+"\n"), 0o644); err != nil {
			return err
		}
	}
	return syncDir(dir)
}

// syncDir flushes a directory so the state survives a power cut.
//
// This board is expected to lose power without warning -- that is the case
// A/B exists for -- and a rename or a write that is still in the page cache
// when the power goes is a trial that cannot be rolled back.
func syncDir(dir string) error {
	f, err := os.Open(dir)
	if err != nil {
		return err
	}
	defer f.Close()
	return f.Sync()
}

// loopBackingFiles lists the files currently backing a loop device.
//
// Read from sysfs rather than by running losetup, so this works in an
// initramfs and needs nothing installed.
func loopBackingFiles() map[string]bool {
	out := map[string]bool{}
	entries, err := filepath.Glob("/sys/block/loop*/loop/backing_file")
	if err != nil {
		return out
	}
	for _, e := range entries {
		b, err := os.ReadFile(e)
		if err != nil {
			continue
		}
		name := strings.TrimSpace(string(b))
		// A backing file that has been unlinked is reported with a suffix.
		name = strings.TrimSuffix(name, " (deleted)")
		out[name] = true
	}
	return out
}

// writeSlotFile installs an image as a slot file.
func (d Disk) writeSlotFile(slot, image string) error {
	dst := filepath.Join(d.Path, slotFileName(slot))

	// Refusing to write a file something is reading.
	//
	// On this layout the root filesystem is loop-mounted from a slot file for
	// the whole uptime, so overwriting the wrong one does not fail an install:
	// it corrupts the running system underneath itself. Install already
	// refuses the active slot, and this is the same rule enforced against what
	// the kernel is actually doing rather than against what the state files
	// claim -- which is what matters if the two ever disagree.
	//
	// EOS solves the same problem by copying its whole 591 MB image aside on
	// every version change. A/B gets it for free by writing the other slot.
	if abs, err := filepath.Abs(dst); err == nil {
		if loopBackingFiles()[abs] {
			return fmt.Errorf("slot %s is loop-mounted right now (%s); writing it would "+
				"corrupt the running system", slot, abs)
		}
	}

	src, err := os.Open(image)
	if err != nil {
		return err
	}
	defer src.Close()
	fi, err := src.Stat()
	if err != nil {
		return err
	}
	if err := checkSquashfs(src); err != nil {
		return err
	}

	// Written beside the target and renamed, so an install interrupted by a
	// power cut leaves the previous slot intact rather than a half-written
	// image that mounts far enough to look plausible.
	tmp := dst + ".new"
	out, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, src); err != nil {
		out.Close()
		os.Remove(tmp)
		return err
	}
	if err := out.Sync(); err != nil {
		out.Close()
		os.Remove(tmp)
		return err
	}
	if err := out.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, dst); err != nil {
		os.Remove(tmp)
		return err
	}
	d.logf("wrote %s (%.1f MiB)\n", dst, float64(fi.Size())/(1<<20))
	return syncDir(d.Path)
}

// checkSquashfs refuses anything the initramfs could not mount.
//
// Catching it here turns a failed trial boot -- which costs a reboot, a
// rollback and a trip to a console to find out why -- into an error at the
// moment somebody typed the wrong filename.
func checkSquashfs(src io.ReaderAt) error {
	magic := make([]byte, 4)
	if _, err := src.ReadAt(magic, 0); err != nil {
		return fmt.Errorf("reading the image: %w", err)
	}
	if string(magic) != "hsqs" {
		return fmt.Errorf("that is not a squashfs image (magic %q, wanted \"hsqs\"): "+
			"the initramfs would refuse it and the trial would roll back", magic)
	}
	return nil
}

// clearSlotOverlay removes the target slot's writable layer.
//
// An install that leaves the old upper layer in place is not an install of the
// image: overlayfs puts that layer on top, file by file, so anything
// hot-patched into the slot earlier keeps winning over what was just written.
// It has produced two silently-wrong installs, where the version reported and
// the binary running disagreed and nothing said so.
//
// Only done where the data filesystem is provably the one belonging to this
// disk. On the file-backed layout it is, because the boot state was just read
// from it. On a partitioned disk being written offline it is not -- the build
// host's own /mnt/data is not the image's data partition -- so it is skipped
// unless the caller names the directory explicitly.
func (d Disk) clearSlotOverlay(slot string) error {
	if !d.fileBacked() && d.Data == "" {
		return nil
	}
	dir := filepath.Join(d.dataDir(), "slot-"+slot)
	if fi, err := os.Stat(dir); err != nil || !fi.IsDir() {
		// Nothing there. A slot that has never booted has no overlay, which is
		// the normal case for a factory install.
		return nil
	}
	for _, sub := range []string{"upper", "work"} {
		p := filepath.Join(dir, sub)
		if _, err := os.Stat(p); err != nil {
			continue
		}
		if err := os.RemoveAll(p); err != nil {
			return fmt.Errorf("clearing slot %s's %s layer: %w", slot, sub, err)
		}
		if err := os.MkdirAll(p, 0o755); err != nil {
			return err
		}
		d.logf("cleared slot %s's %s layer\n", slot, sub)
	}
	return syncDir(dir)
}
