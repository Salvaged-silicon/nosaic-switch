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

// The data partition is waited for, not probed once. On the AS5610 the disk is
// a USB DOM that attaches a second or two into the boot; the one-shot lookup
// lost that race and the switch came up stateless with its real configuration
// sitting on a partition that appeared a moment later.
//
// This runs the script's own block, not a copy of it, against stubs: findfs
// finds the partition only from a given round on, and sleep advances the
// round. So it measures what the loop does, not what its text says.
func TestDataPartitionIsWaitedFor(t *testing.T) {
	sh, err := exec.LookPath("dash")
	if err != nil {
		if sh, err = exec.LookPath("sh"); err != nil {
			t.Skip("no POSIX shell available")
		}
	}
	start := strings.Index(initScript, "PERSIST=no\n_dwaited=0")
	if start < 0 {
		t.Fatal("the data-partition wait loop is gone from the init script")
	}
	end := strings.Index(initScript[start:], "\ndone\n")
	block := initScript[start : start+end+len("\ndone\n")]

	stubs := `
ROUND=0
findfs() { [ -n "$DATA_AT" ] && [ "$ROUND" -ge "$DATA_AT" ] && echo /dev/sda4; }
mount() {
    case "$*" in
        *tmpfs*) echo "MOUNT tmpfs"; return 0 ;;
        *loop*)  echo "MOUNT loop"; return 0 ;;
        *ext4*)  echo "MOUNT ext4"; return 0 ;;
    esac
    return 1
}
sleep() { ROUND=$((ROUND + 1)); }
mkdir() { :; }
fail() { echo "FAIL $*"; exit 1; }
FLASH=""
mount_flash() {
    [ -n "$FLASH_AT" ] && [ "$ROUND" -ge "$FLASH_AT" ] || return 1
    FLASH=$FLASHDIR
    return 0
}
`
	cases := []struct {
		name, env, want string
		persist         bool
	}{
		{"disk attaches at 3s", "DATA_AT=3", "data partition mounted (/dev/sda4) after 3s", true},
		{"disk already there", "DATA_AT=0", "data partition mounted (/dev/sda4)\n", true},
		{"flash with a data image at 2s", "FLASH_AT=2 IMG=1", "data image mounted", true},
		{"flash with no data image", "FLASH_AT=0", "booting stateless", false},
		{"nothing at all", "", "no data partition after 15s; booting stateless", false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			dir := t.TempDir()
			script := c.env + "\nFLASHDIR=" + dir + "\n" +
				`[ -n "$IMG" ] && : > "$FLASHDIR/nosaic-data.img"` + "\n" +
				stubs + block + `echo "PERSIST=$PERSIST ROUND=$ROUND"` + "\n"
			out, err := exec.Command(sh, "-c", script).CombinedOutput()
			if err != nil {
				t.Fatalf("%v\n%s", err, out)
			}
			got := string(out)
			if !strings.Contains(got, c.want) {
				t.Errorf("want %q in:\n%s", c.want, got)
			}
			want := "PERSIST=no"
			if c.persist {
				want = "PERSIST=yes"
			}
			if !strings.Contains(got, want) {
				t.Errorf("want %s in:\n%s", want, got)
			}
			if c.name == "flash with no data image" && !strings.Contains(got, "ROUND=0") {
				t.Errorf("a flash with no data image waited anyway:\n%s", got)
			}
		})
	}
}
