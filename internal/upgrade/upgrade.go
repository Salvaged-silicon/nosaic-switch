// Package upgrade installs an image into a slot and manages the boot pointer.
//
// The same operations serve two callers: a factory writing a disk image before
// anything has booted, and a switch upgrading itself. Both write to the slot
// that is not running and mark it for a trial; neither ever overwrites the
// slot it booted from, which is what makes an upgrade reversible.
//
// State lives on the boot partition as three small files:
//
//	boot/active   the committed slot, booted when nothing else applies
//	boot/trial    a slot on trial, present only between install and commit
//	boot/tries    how many times the trial has been attempted
//
// They live on the boot partition rather than the data partition because that
// filesystem carries no journal: ext4 replays its journal when the kernel
// mounts it, silently undoing anything written offline.
//
// Plain files rather than a binary format on purpose: the state a switch will
// fall back on should be readable and repairable with the tools present in an
// initramfs.
package upgrade

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

// State is the boot pointer.
type State struct {
	Active string
	Trial  string
	Tries  int
}

// Disk is a NOSaic disk, addressed offline by file or online by device.
//
// Path names a block device or a disk image when the slots are partitions,
// and a directory when they are files on the bootloader's own filesystem.
// See filebacked.go for why that second layout exists.
type Disk struct {
	Path string

	// Data is the mounted persistent filesystem, where boot state and the
	// per-slot overlays live. Empty means /mnt/data, which is correct on a
	// running switch and wrong on a build host -- so the overlay is only
	// cleared when this is known to belong to this disk.
	Data string

	// Files says the slots and state are plain files on a mounted filesystem.
	// Normally inferred from Path being a directory; set explicitly when the
	// layout is known but the bootloader's filesystem is not mounted.
	Files bool

	// Log receives progress. Nil is silent, which is what the offline
	// callers want.
	Log io.Writer
}

type partition struct {
	Start int64  `json:"start"`
	Size  int64  `json:"size"`
	Name  string `json:"name"`
}

func (d Disk) partitions() ([]partition, error) {
	out, err := exec.Command("sfdisk", "--json", d.Path).Output()
	if err != nil {
		return nil, fmt.Errorf("reading the partition table of %s: %w", d.Path, err)
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

// slotIndex maps a slot letter to its partition index.
func slotIndex(slot string) (int, error) {
	switch slot {
	case "a":
		return 1, nil
	case "b":
		return 2, nil
	}
	return 0, fmt.Errorf("unknown slot %q: slots are a and b", slot)
}

// bootIndex is the partition holding the slot pointer. It carries no journal,
// so an offline edit with debugfs is not reverted when the kernel next mounts
// it -- which is exactly what happens on ext4, silently.
const bootIndex = 0

// Install writes an image into a slot and marks it for trial.
//
// It refuses to write to the currently active slot. That is not a convenience
// check: overwriting the running image is precisely the failure A/B exists to
// prevent, and an installer that allows it has quietly removed the safety net.
func Install(d Disk, slot, image string) error {
	st, err := Status(d)
	if err != nil {
		return err
	}
	if slot == st.Active {
		return fmt.Errorf("slot %s is the active slot; installing into it would overwrite "+
			"the running image and leave nothing to roll back to", slot)
	}
	idx, err := slotIndex(slot)
	if err != nil {
		return err
	}

	// Slots as files, for a board whose bootloader owns the whole disk.
	if d.fileBacked() {
		if err := d.writeSlotFile(slot, image); err != nil {
			return err
		}
		if err := d.clearSlotOverlay(slot); err != nil {
			return err
		}
		return d.writeState(map[string]string{"trial": slot, "tries": "0"})
	}

	parts, err := d.partitions()
	if err != nil {
		return err
	}
	if idx >= len(parts) {
		return fmt.Errorf("this disk has no slot %s", slot)
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
	limit := parts[idx].Size * 512
	if fi.Size() > limit {
		return fmt.Errorf("the image is %.1f MiB and slot %s is %.1f MiB",
			float64(fi.Size())/(1<<20), slot, float64(limit)/(1<<20))
	}

	dst, err := os.OpenFile(d.Path, os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	defer dst.Close()
	if _, err := dst.Seek(parts[idx].Start*512, io.SeekStart); err != nil {
		return err
	}
	if _, err := io.Copy(dst, src); err != nil {
		return err
	}
	if err := dst.Sync(); err != nil {
		return err
	}

	if err := d.clearSlotOverlay(slot); err != nil {
		return err
	}

	// Marked as a trial, never as active. Nothing becomes the committed choice
	// until it has booted and said it is healthy.
	if err := d.writeState(map[string]string{"trial": slot, "tries": "0"}); err != nil {
		return err
	}
	return nil
}

// Commit makes the slot on trial the committed one.
//
// A trial that is never committed rolls back, which is the safe default and
// the whole point. But the only thing that committed one was the boot
// self-test, and that runs solely when nosaic.selftest is on the kernel
// command line -- a QEMU harness flag that no real switch sets. So on hardware
// a good upgrade rolled back exactly like a bad one, three boots later, with
// nothing saying why.
//
// This is the explicit form, for an operator or a fleet tool that has decided
// the new image is good. The self-test remains the automatic form.
func Commit(d Disk) (string, error) {
	st, err := Status(d)
	if err != nil {
		return "", err
	}
	if st.Trial == "" {
		return "", fmt.Errorf("no trial is in progress: slot %s is already the committed one", st.Active)
	}
	slot := st.Trial
	// active first, then clear the trial. In the other order a power cut
	// between the two writes leaves neither a trial nor the new active slot,
	// and the switch quietly boots the old one.
	if err := d.writeState(map[string]string{"active": slot}); err != nil {
		return "", err
	}
	if err := d.writeState(map[string]string{"trial": "", "tries": ""}); err != nil {
		return "", err
	}
	return slot, nil
}

// Status reads the boot pointer.
func Status(d Disk) (State, error) {
	st := State{Active: "a"}
	files, err := d.readState()
	if err != nil {
		return st, err
	}
	if v := strings.TrimSpace(files["active"]); v != "" {
		st.Active = v
	}
	st.Trial = strings.TrimSpace(files["trial"])
	if n, err := strconv.Atoi(strings.TrimSpace(files["tries"])); err == nil {
		st.Tries = n
	}
	return st, nil
}

// withData gives fn a path to the data filesystem.
//
// debugfs is used rather than mounting, because mounting needs privileges an
// unprivileged build has no business requiring, and because the same code then
// works offline on an image file and online on a block device.
//
// debugfs cannot address a filesystem at an offset inside a larger file, so
// when the target is a whole disk the partition is extracted, operated on, and
// written back. When the target is already a filesystem — a partition device
// on a running switch — it is used directly and nothing is copied.
func (d Disk) withData(write bool, fn func(path string) error) error {
	parts, err := d.partitions()
	if err != nil || len(parts) <= bootIndex {
		// No partition table: this is the filesystem itself.
		return fn(d.Path)
	}
	start := parts[bootIndex].Start * 512
	size := parts[bootIndex].Size * 512

	tmp, err := os.CreateTemp("", "nosaic-data-*.ext4")
	if err != nil {
		return err
	}
	path := tmp.Name()
	defer os.Remove(path)

	disk, err := os.Open(d.Path)
	if err != nil {
		tmp.Close()
		return err
	}
	if _, err := io.Copy(tmp, io.NewSectionReader(disk, start, size)); err != nil {
		disk.Close()
		tmp.Close()
		return err
	}
	disk.Close()
	if err := tmp.Close(); err != nil {
		return err
	}

	if err := fn(path); err != nil {
		return err
	}
	if !write {
		return nil
	}

	src, err := os.Open(path)
	if err != nil {
		return err
	}
	defer src.Close()
	dst, err := os.OpenFile(d.Path, os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	defer dst.Close()
	if _, err := dst.Seek(start, io.SeekStart); err != nil {
		return err
	}
	if _, err := io.CopyN(dst, src, size); err != nil && err != io.EOF {
		return err
	}
	return dst.Sync()
}

func debugfsRun(fsPath string, write bool, request string) (string, error) {
	args := []string{}
	if write {
		args = append(args, "-w")
	}
	args = append(args, "-R", request, fsPath)
	out, err := exec.Command("debugfs", args...).CombinedOutput()
	if err != nil {
		return string(out), fmt.Errorf("debugfs %q: %w\n%s", request, err, out)
	}
	// debugfs reports several failures on stdout with a zero exit status, so
	// the output has to be inspected rather than trusted.
	if strings.Contains(string(out), "File not found") {
		return string(out), os.ErrNotExist
	}
	return string(out), nil
}

func (d Disk) readState() (map[string]string, error) {
	if d.fileBacked() {
		return readStateDir(filepath.Join(d.dataDir(), "boot"))
	}
	out := map[string]string{}
	err := d.withData(false, func(fs string) error {
		for _, name := range []string{"active", "trial", "tries"} {
			tmp, err := os.CreateTemp("", "nosaic-state-")
			if err != nil {
				return err
			}
			path := tmp.Name()
			tmp.Close()
			os.Remove(path)

			// Absent is a legitimate state, not an error: a disk that has
			// never been upgraded has no trial recorded.
			if _, err := debugfsRun(fs, false, fmt.Sprintf("dump /boot/%s %s", name, path)); err != nil {
				continue
			}
			if b, err := os.ReadFile(path); err == nil {
				out[name] = string(b)
			}
			os.Remove(path)
		}
		return nil
	})
	return out, err
}

func (d Disk) writeState(files map[string]string) error {
	if d.fileBacked() {
		return writeStateDir(filepath.Join(d.dataDir(), "boot"), files)
	}
	return d.withData(true, func(fs string) error {
		for name, content := range files {
			tmp, err := os.CreateTemp("", "nosaic-state-")
			if err != nil {
				return err
			}
			if _, err := tmp.WriteString(content + "\n"); err != nil {
				tmp.Close()
				return err
			}
			tmp.Close()
			defer os.Remove(tmp.Name())

			// Removing first: debugfs's write does not replace an existing
			// file, it fails, and a state file that silently keeps its old
			// value is how a committed upgrade appears not to have happened.
			_, _ = debugfsRun(fs, true, fmt.Sprintf("rm /boot/%s", name))
			if content == "" {
				// An empty value means "clear this", not "write nothing".
				// Leaving a zero-length trial behind is a trial that never
				// expires, which is the opposite of committing it.
				continue
			}
			if _, err := debugfsRun(fs, true, fmt.Sprintf("write %s boot/%s", tmp.Name(), name)); err != nil {
				return err
			}
		}
		return nil
	})
}
