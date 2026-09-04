package upgrade

import (
	"os"
	"path/filepath"
	"testing"
)

// newFileBacked builds the layout a board like the 7050SX2 has: a flash
// directory holding slot files, and a mounted data filesystem holding boot
// state and the per-slot overlays.
func newFileBacked(t *testing.T, active string) (Disk, string, string) {
	t.Helper()
	root := t.TempDir()
	flash := filepath.Join(root, "flash")
	data := filepath.Join(root, "data")
	for _, d := range []string{flash, filepath.Join(data, "boot")} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(data, "boot", "active"), []byte(active+"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	return Disk{Path: flash, Data: data}, flash, data
}

func squashfsImage(t *testing.T, dir, name string, size int) string {
	t.Helper()
	b := make([]byte, size)
	copy(b, "hsqs")
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, b, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestInstallWritesTheSlotFileTheInitramfsLooksFor(t *testing.T) {
	d, flash, _ := newFileBacked(t, "a")
	img := squashfsImage(t, t.TempDir(), "rootfs.sqsh", 4096)

	if err := Install(d, "b", img); err != nil {
		t.Fatalf("install: %v", err)
	}
	// The name has to match slotdev() in the initramfs, or the switch boots
	// and cannot find what was just installed.
	want := filepath.Join(flash, "nosaic-slot-b.sqsh")
	if _, err := os.Stat(want); err != nil {
		t.Fatalf("slot file not written where the initramfs looks: %v", err)
	}
	if _, err := os.Stat(want + ".new"); !os.IsNotExist(err) {
		t.Error("the temporary file was left behind; an interrupted install must not leave one")
	}
	st, err := Status(d)
	if err != nil {
		t.Fatal(err)
	}
	if st.Trial != "b" || st.Active != "a" {
		t.Errorf("after install: active=%q trial=%q, want active=a trial=b", st.Active, st.Trial)
	}
}

// Overwriting the running image is the one thing A/B exists to prevent.
func TestInstallRefusesTheActiveSlot(t *testing.T) {
	d, flash, _ := newFileBacked(t, "a")
	img := squashfsImage(t, t.TempDir(), "rootfs.sqsh", 4096)

	if err := Install(d, "a", img); err == nil {
		t.Fatal("installing into the active slot was allowed")
	}
	if _, err := os.Stat(filepath.Join(flash, "nosaic-slot-a.sqsh")); !os.IsNotExist(err) {
		t.Error("the active slot file was written despite the refusal")
	}
}

// The bug this package has had twice: the previous slot's writable layer
// survives an install and shadows the new image, file by file, so a binary
// hot-patched there keeps winning and nothing says so.
func TestInstallClearsTheTargetSlotsOverlay(t *testing.T) {
	d, _, data := newFileBacked(t, "a")
	img := squashfsImage(t, t.TempDir(), "rootfs.sqsh", 4096)

	stale := filepath.Join(data, "slot-b", "upper", "usr", "sbin")
	if err := os.MkdirAll(stale, 0o755); err != nil {
		t.Fatal(err)
	}
	shadow := filepath.Join(stale, "nosd-td2p")
	if err := os.WriteFile(shadow, []byte("an old binary"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(data, "slot-b", "work", "work"), 0o755); err != nil {
		t.Fatal(err)
	}

	if err := Install(d, "b", img); err != nil {
		t.Fatalf("install: %v", err)
	}
	if _, err := os.Stat(shadow); !os.IsNotExist(err) {
		t.Error("slot b's overlay survived the install; the new image is shadowed by the old one")
	}
	// Still usable as an overlay afterwards.
	for _, sub := range []string{"upper", "work"} {
		p := filepath.Join(data, "slot-b", sub)
		if fi, err := os.Stat(p); err != nil || !fi.IsDir() {
			t.Errorf("slot b's %s layer was removed but not recreated", sub)
		}
	}
}

// Clearing the wrong slot's overlay would destroy the state we roll back to.
func TestInstallLeavesTheOtherSlotsOverlayAlone(t *testing.T) {
	d, _, data := newFileBacked(t, "a")
	img := squashfsImage(t, t.TempDir(), "rootfs.sqsh", 4096)

	keep := filepath.Join(data, "slot-a", "upper", "etc")
	if err := os.MkdirAll(keep, 0o755); err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(keep, "keepme")
	if err := os.WriteFile(marker, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := Install(d, "b", img); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(marker); err != nil {
		t.Error("installing into b disturbed slot a's overlay, which is what a rollback returns to")
	}
}

// A wrong file caught here costs a typo; caught at boot it costs a reboot, a
// rollback and a trip to a console.
func TestInstallRefusesSomethingThatIsNotASquashfs(t *testing.T) {
	d, flash, _ := newFileBacked(t, "a")
	dir := t.TempDir()
	bad := filepath.Join(dir, "notanimage.tar")
	if err := os.WriteFile(bad, []byte("this is not a filesystem"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := Install(d, "b", bad); err == nil {
		t.Fatal("a non-squashfs image was accepted")
	}
	if _, err := os.Stat(filepath.Join(flash, "nosaic-slot-b.sqsh")); !os.IsNotExist(err) {
		t.Error("the slot was written even though the image was refused")
	}
}

// A trial is cleared by removing the file. Writing an empty one leaves a trial
// the initramfs would keep honouring.
func TestClearingATrialRemovesTheFile(t *testing.T) {
	d, _, data := newFileBacked(t, "a")
	if err := d.writeState(map[string]string{"trial": "b", "tries": "1"}); err != nil {
		t.Fatal(err)
	}
	if err := d.writeState(map[string]string{"trial": "", "tries": ""}); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"trial", "tries"} {
		if _, err := os.Stat(filepath.Join(data, "boot", name)); !os.IsNotExist(err) {
			t.Errorf("%s still exists after being cleared", name)
		}
	}
	st, err := Status(d)
	if err != nil {
		t.Fatal(err)
	}
	if st.Trial != "" {
		t.Errorf("trial reads back as %q after being cleared", st.Trial)
	}
}

// A switch that has never been upgraded has no trial recorded, and that is a
// state rather than a fault.
func TestStatusDefaultsToSlotAWithNoStateAtAll(t *testing.T) {
	root := t.TempDir()
	flash := filepath.Join(root, "flash")
	if err := os.MkdirAll(flash, 0o755); err != nil {
		t.Fatal(err)
	}
	d := Disk{Path: flash, Data: filepath.Join(root, "data")}
	st, err := Status(d)
	if err != nil {
		t.Fatalf("status on a fresh board: %v", err)
	}
	if st.Active != "a" || st.Trial != "" || st.Tries != 0 {
		t.Errorf("fresh board reads active=%q trial=%q tries=%d, want a/none/0", st.Active, st.Trial, st.Tries)
	}
}

// The build host's own /mnt/data is not a disk image's data partition, so an
// offline install must not touch it.
func TestOfflineInstallDoesNotTouchTheHostsDataDirectory(t *testing.T) {
	d := Disk{Path: filepath.Join(t.TempDir(), "disk.img")} // a file, not a directory
	if d.fileBacked() {
		t.Fatal("a missing path was treated as a file-backed layout")
	}
	if err := d.clearSlotOverlay("b"); err != nil {
		t.Fatalf("clearing on a partitioned disk with no Data set should be a no-op: %v", err)
	}
}

// A good upgrade on real hardware used to roll back exactly like a bad one:
// the only thing that committed was the boot self-test, which runs solely
// under the QEMU harness.
func TestCommitMakesTheTrialTheBootedSlot(t *testing.T) {
	d, _, data := newFileBacked(t, "a")
	img := squashfsImage(t, t.TempDir(), "rootfs.sqsh", 4096)
	if err := Install(d, "b", img); err != nil {
		t.Fatal(err)
	}

	slot, err := Commit(d)
	if err != nil {
		t.Fatalf("commit: %v", err)
	}
	if slot != "b" {
		t.Errorf("committed %q, want b", slot)
	}
	st, err := Status(d)
	if err != nil {
		t.Fatal(err)
	}
	if st.Active != "b" {
		t.Errorf("active is %q after committing b", st.Active)
	}
	if st.Trial != "" || st.Tries != 0 {
		t.Errorf("trial state survived the commit: trial=%q tries=%d", st.Trial, st.Tries)
	}
	// A leftover trial file is a trial that never expires.
	if _, err := os.Stat(filepath.Join(data, "boot", "trial")); !os.IsNotExist(err) {
		t.Error("the trial file still exists after commit")
	}
}

func TestCommitWithNothingOnTrialIsAnError(t *testing.T) {
	d, _, _ := newFileBacked(t, "a")
	if _, err := Commit(d); err == nil {
		t.Fatal("committing with no trial in progress was allowed")
	}
}

// Install into b, commit, then install into a: the slot that is now active
// must be the one refused.
func TestTheRefusedSlotFollowsTheCommit(t *testing.T) {
	d, _, _ := newFileBacked(t, "a")
	img := squashfsImage(t, t.TempDir(), "rootfs.sqsh", 4096)
	if err := Install(d, "b", img); err != nil {
		t.Fatal(err)
	}
	if _, err := Commit(d); err != nil {
		t.Fatal(err)
	}
	if err := Install(d, "b", img); err == nil {
		t.Error("slot b is active after the commit and must now be refused")
	}
	if err := Install(d, "a", img); err != nil {
		t.Errorf("slot a is inactive after the commit and should be installable: %v", err)
	}
}
