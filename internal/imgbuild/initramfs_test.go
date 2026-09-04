package imgbuild

import (
	"os/exec"
	"strings"
	"testing"
)

// The init script is shell living inside a Go string, so the Go compiler
// cannot see a syntax error in it and the failure surfaces as a switch that
// stops in an initramfs shell in a rack. dash is the closest thing on a build
// host to the busybox ash that actually runs it.
func TestInitScriptIsValidPOSIXShell(t *testing.T) {
	sh, err := exec.LookPath("dash")
	if err != nil {
		if sh, err = exec.LookPath("sh"); err != nil {
			t.Skip("no POSIX shell available to check with")
		}
	}
	cmd := exec.Command(sh, "-n")
	cmd.Stdin = strings.NewReader(initScript)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("the generated init script is not valid shell: %v\n%s", err, out)
	}
}

// slotdev() looks for a slot file on the bootloader's filesystem before it
// guesses at partition numbers, and the order is load-bearing rather than
// stylistic.
//
// On an Arista, /dev/mmcblk0p2 exists and belongs to the vendor -- it is a
// 1 MB diagnostics partition. With the guess first, slotdev returned that
// device, the boot got as far as mounting it as squashfs, and failed there.
// A guess that lands on a real device belonging to someone else is worse than
// one that finds nothing, because it looks like a successful lookup.
func TestSlotFileIsPreferredOverTheNumericPartitionGuess(t *testing.T) {
	body := slotdevBody(t)
	file := strings.Index(body, `[ -f "$FLASH/$want.sqsh" ]`)
	guess := strings.Index(body, "for d in /dev/vda /dev/sda /dev/mmcblk0p")
	if file < 0 || guess < 0 {
		t.Fatalf("slotdev no longer has both lookups: file=%d guess=%d", file, guess)
	}
	if file > guess {
		t.Error("slotdev guesses at partition numbers before looking for a slot file; " +
			"on a board where the bootloader owns the disk that guess finds the vendor's partition")
	}
}

// A trial slot whose image is corrupt must still resolve to something, so the
// boot reaches the mount, fails there, and rolls back. Returning nothing would
// make the initramfs fail hard instead, turning a recoverable bad upgrade into
// a switch that will not boot.
func TestNumericGuessStillReturnsADeviceThatHoldsNoImage(t *testing.T) {
	body := slotdevBody(t)
	if !strings.Contains(body, `echo "$first"`) {
		t.Error("slotdev no longer falls back to the first device that exists; " +
			"a trial slot with a corrupt image would fail the boot instead of rolling back")
	}
}

// mount_flash is called from slotdev, which runs inside "$( )". A mount made
// there survives into the parent while the FLASH variable set beside it does
// not -- so the second call, which is the rollback path, found the device
// already mounted, failed, and reported no slot file on a board whose slot
// file was sitting right there.
func TestMountFlashIsIdempotentAcrossSubshells(t *testing.T) {
	body := funcBody(t, "mount_flash")
	already := strings.Index(body, "/proc/mounts")
	loop := strings.Index(body, "for _d in")
	if already < 0 {
		t.Fatal("mount_flash no longer checks whether the flash is already mounted")
	}
	if already > loop {
		t.Error("mount_flash tries to mount before checking whether it already did")
	}
}

// An unresolvable trial slot gets the same verdict as an unmountable one.
func TestAnUnresolvableTrialSlotRollsBackRatherThanFailingTheBoot(t *testing.T) {
	if !strings.Contains(initScript, "could not be found; returning to") {
		t.Error("a trial slot that resolves to nothing still fails the boot instead of rolling back")
	}
}

func slotdevBody(t *testing.T) string { return funcBody(t, "slotdev") }

func funcBody(t *testing.T, name string) string {
	t.Helper()
	start := strings.Index(initScript, name+"() {")
	if start < 0 {
		t.Fatalf("the init script no longer defines %s()", name)
	}
	end := strings.Index(initScript[start:], "\n}\n")
	if end < 0 {
		t.Fatalf("cannot find the end of %s()", name)
	}
	return initScript[start : start+end]
}

// Confirming a trial must not run inside the s6 bundle change.
//
// trial-confirm is a oneshot, and "s6-rc -u change default" waits for every
// oneshot with a 120-second budget. Confirmation waits up to five minutes for
// a datapath, so run inline a trial whose datapath never comes up would hold
// the bundle past its deadline; the init script reads that as the service
// database failing and drops to a rescue shell. A switch that was merely
// declining an upgrade would look catastrophically broken.
func TestTrialConfirmationDetachesFromTheBoot(t *testing.T) {
	if !strings.Contains(confirmScript, "setsid") {
		t.Error("the trial confirmation no longer detaches; a slow decline will " +
			"hold the s6 bundle past its deadline and drop the board to a rescue shell")
	}
	if !strings.Contains(confirmScript, "nosaic upgrade confirm") {
		t.Error("the trial confirmation script no longer runs the confirm command")
	}
}
