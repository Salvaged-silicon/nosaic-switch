package imgbuild

import (
	"io"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

// The build must refuse an image whose declared privilege is not really there.
//
// This is the exact defect that shipped: base/identity.yml said `privilege:
// sudo`, no profile packaged sudo, root was locked, and every minimal image
// went out with no path to root at all. A declaration in one file and a
// package list in another cannot contradict each other unless something
// compares them.
func TestBuildRefusesADeclaredPrivilegeThatIsMissing(t *testing.T) {
	root := t.TempDir()
	err := writePrivilege(root, "admin", "doas", io.Discard)
	if err == nil {
		t.Fatal("an image with no doas was accepted")
	}
	if !strings.Contains(err.Error(), "not in the image") {
		t.Errorf("error should say the binary is absent, got: %v", err)
	}
}

// A privilege helper that is present but not setuid runs, fails, and looks
// like a configuration problem. The build should say so instead.
func TestBuildRefusesAPrivilegeHelperThatIsNotSetuid(t *testing.T) {
	root := t.TempDir()
	bin := filepath.Join(root, "usr/bin")
	if err := os.MkdirAll(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	doas := filepath.Join(bin, "doas")
	if err := os.WriteFile(doas, []byte("x"), 0o755); err != nil {
		t.Fatal(err)
	}

	err := writePrivilege(root, "admin", "doas", io.Discard)
	if err == nil {
		t.Fatal("a doas that is not setuid was accepted")
	}
	if !strings.Contains(err.Error(), "setuid") {
		t.Errorf("error should name the missing setuid bit, got: %v", err)
	}

	// Setuid to somebody who is not root elevates to that somebody, which is
	// not what the image asked for and is worth its own message. The real
	// build runs as root in the builder container, so this is reachable only
	// by a build that has gone wrong.
	if err := syscall.Chmod(doas, 0o4755); err != nil {
		t.Skipf("cannot set setuid here: %v", err)
	}
	if os.Geteuid() != 0 {
		err = writePrivilege(root, "admin", "doas", io.Discard)
		if err == nil || !strings.Contains(err.Error(), "rather than root") {
			t.Errorf("setuid to a non-root uid should be rejected, got: %v", err)
		}
		return
	}
	if err := writePrivilege(root, "admin", "doas", io.Discard); err != nil {
		t.Fatalf("a setuid-root doas should be accepted: %v", err)
	}
}

// The generated config must actually permit the account. A doas.conf that
// parses and permits nobody is a privilege path that exists and refuses.
func TestGeneratedPrivilegeConfigPermitsTheAccount(t *testing.T) {
	for _, tc := range []struct{ priv, file, want string }{
		{"doas", "etc/doas.conf", "permit persist admin as root"},
		{"sudo", "etc/sudoers", "admin\tALL=(ALL:ALL) ALL"},
	} {
		root := t.TempDir()
		bin := filepath.Join(root, "usr/bin")
		if err := os.MkdirAll(bin, 0o755); err != nil {
			t.Fatal(err)
		}
		// The binary check is exercised above; here only the config matters,
		// so tolerate its failure and read what was written.
		if err := os.WriteFile(filepath.Join(bin, tc.priv), []byte("x"), 0o755); err != nil {
			t.Fatal(err)
		}
		_ = writePrivilege(root, "admin", tc.priv, io.Discard)

		b, err := os.ReadFile(filepath.Join(root, tc.file))
		if err != nil {
			t.Errorf("%s: %s was not written: %v", tc.priv, tc.file, err)
			continue
		}
		if !strings.Contains(string(b), tc.want) {
			t.Errorf("%s: %s does not contain %q:\n%s", tc.priv, tc.file, tc.want, b)
		}
	}
}
