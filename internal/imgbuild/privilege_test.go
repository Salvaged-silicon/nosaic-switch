package imgbuild

import (
	"bytes"
	"io"
	"os"
	"os/exec"
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
		{"doas", "etc/doas.conf", "permit nopass admin as root"},
		{"sudo", "etc/sudoers", "admin\tALL=(ALL:ALL) NOPASSWD: ALL"},
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

// The sticky bit must survive onto /tmp.
//
// os.Chmod takes an os.FileMode, where sticky is 1<<20 and not 0o1000, so
// os.Chmod(p, 0o1777) yields 0777: world-writable with no sticky bit, letting
// any user delete another's files. It is invisible to testing because 0777
// passes every "is /tmp writable" check there is -- it was caught by reading
// `ls -ld /tmp` on the switch, not by a test.
func TestStickyAndSetuidBitsSurviveChmodRaw(t *testing.T) {
	dir := t.TempDir()
	for _, mode := range []uint32{0o1777, 0o700, 0o4755, 0o2755} {
		p := filepath.Join(dir, "x")
		if err := os.WriteFile(p, []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
		if err := chmodRaw(p, mode); err != nil {
			t.Fatalf("chmodRaw %04o: %v", mode, err)
		}
		fi, err := os.Stat(p)
		if err != nil {
			t.Fatal(err)
		}
		if got := fi.Sys().(*syscall.Stat_t).Mode & 0o7777; got != mode {
			t.Errorf("chmodRaw(%04o) produced %04o", mode, got)
		}
		if err := os.Remove(p); err != nil {
			t.Fatal(err)
		}
	}
}

// The sudo shim must forward what it claims to forward and refuse the rest by
// name. The whole reason it is allowed to exist instead of packaging sudo is
// that it never silently does something other than what was asked -- a shim
// that dropped an unrecognised flag and ran the command anyway would be worse
// than the "sudo: not found" it replaces, because that at least told the
// truth. So this runs the generated script against a stand-in doas that just
// prints its arguments.
func TestSudoShimForwardsAndRefuses(t *testing.T) {
	root := t.TempDir()
	bin := filepath.Join(root, "usr/bin")
	if err := os.MkdirAll(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(bin, "doas"), []byte("x"), 0o755); err != nil {
		t.Fatal(err)
	}
	_ = writePrivilege(root, "admin", "doas", io.Discard)

	shim := filepath.Join(bin, "sudo")
	if _, err := os.Stat(shim); err != nil {
		t.Fatalf("the shim was not written: %v", err)
	}
	// A doas that reports what it was handed, earlier in PATH than anything real.
	stub := t.TempDir()
	if err := os.WriteFile(filepath.Join(stub, "doas"),
		[]byte("#!/bin/sh\necho \"doas:$*\"\n"), 0o755); err != nil {
		t.Fatal(err)
	}

	for _, tc := range []struct {
		name string
		args []string
		want string // expected stdout, when the shim should forward
		deny string // expected on stderr, when it should refuse
	}{
		{name: "plain", args: []string{"ls", "-l"}, want: "doas:ls -l"},
		{name: "non-interactive", args: []string{"-n", "id"}, want: "doas:-n id"},
		{name: "user long", args: []string{"-u", "frr", "id"}, want: "doas:-u frr id"},
		{name: "user attached", args: []string{"-ufrr", "id"}, want: "doas:-u frr id"},
		{name: "shell", args: []string{"-s"}, want: "doas:-s"},
		{name: "ddash", args: []string{"--", "ls"}, want: "doas:ls"},
		// The command's own flags must reach it untouched, not be eaten.
		{name: "command flags", args: []string{"nosaic", "show", "--json"}, want: "doas:nosaic show --json"},
		// sudo-only flags: refused by name rather than dropped.
		{name: "preserve env", args: []string{"-E", "make"}, deny: "-E is not supported"},
		{name: "login shell", args: []string{"-i"}, deny: "-i is not supported"},
		{name: "list", args: []string{"-l"}, deny: "-l is not supported"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cmd := exec.Command("/bin/sh", append([]string{shim}, tc.args...)...)
			cmd.Env = append(os.Environ(), "PATH="+stub+":"+os.Getenv("PATH"))
			var out, errb bytes.Buffer
			cmd.Stdout, cmd.Stderr = &out, &errb
			err := cmd.Run()

			if tc.deny != "" {
				if err == nil {
					t.Errorf("%v should have been refused, got stdout %q", tc.args, out.String())
				}
				if !strings.Contains(errb.String(), tc.deny) {
					t.Errorf("%v: refusal should name the flag (%q), got: %s",
						tc.args, tc.deny, errb.String())
				}
				// The point of refusing: the command must not have run.
				if strings.Contains(out.String(), "doas:") {
					t.Errorf("%v was refused but still ran doas: %s", tc.args, out.String())
				}
				return
			}
			if err != nil {
				t.Fatalf("%v: %v (stderr: %s)", tc.args, err, errb.String())
			}
			if got := strings.TrimSpace(out.String()); got != tc.want {
				t.Errorf("%v forwarded %q, want %q", tc.args, got, tc.want)
			}
		})
	}
}
