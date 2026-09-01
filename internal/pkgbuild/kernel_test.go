package pkgbuild

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A symbol a fragment turns off can be turned back on by another symbol that
// selects it. That happened for real: mpc85xx_smp_defconfig sets
// CONFIG_PHYS_64BIT, the AS5610 must have it off, and a board that boots with
// it on prints nothing at all -- no panic, no console. The check existed but
// skipped every line beginning with #, which is every request of this kind.
func TestVerifyFragmentChecksSymbolsAskedToBeOff(t *testing.T) {
	dir := t.TempDir()
	write := func(config string) {
		if err := os.WriteFile(filepath.Join(dir, ".config"), []byte(config), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	fragment := []byte("# CONFIG_PHYS_64BIT is not set\nCONFIG_SQUASHFS=y\n")

	write("CONFIG_PHYS_64BIT=y\nCONFIG_SQUASHFS=y\n")
	err := verifyFragment(dir, fragment)
	if err == nil {
		t.Fatal("a symbol asked to be off and left on was accepted")
	}
	if !strings.Contains(err.Error(), "PHYS_64BIT") {
		t.Fatalf("error names the wrong symbol: %v", err)
	}

	// The two ways kconfig spells "off", both of which must pass.
	for _, off := range []string{
		"# CONFIG_PHYS_64BIT is not set\nCONFIG_SQUASHFS=y\n",
		"CONFIG_PHYS_64BIT=n\nCONFIG_SQUASHFS=y\n",
	} {
		write(off)
		if err := verifyFragment(dir, fragment); err != nil {
			t.Errorf("a correctly disabled symbol was rejected: %v", err)
		}
	}

	// An ordinary comment is still an ordinary comment.
	write("CONFIG_SQUASHFS=y\n")
	if err := verifyFragment(dir, []byte("# just explaining myself\nCONFIG_SQUASHFS=y\n")); err != nil {
		t.Errorf("a comment was read as a request: %v", err)
	}
}
