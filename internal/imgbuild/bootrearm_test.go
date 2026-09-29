/* SPDX-License-Identifier: Apache-2.0 */
package imgbuild

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
)

func rearmScript(t *testing.T) (*board.Board, string) {
	t.Helper()
	b, err := board.Load(filepath.Join("..", "..", "platform", "cisco-n3172tq", "board.yml"))
	if err != nil {
		t.Fatal(err)
	}
	src, err := os.ReadFile(filepath.Join(filepath.Dir(b.Path), b.BootRearm))
	if err != nil {
		t.Fatalf("boot_rearm script: %v", err)
	}
	return b, string(src)
}

// The Nexus 3172TQ boots unattended only if the DOS table, the failing loader
// menu line and this CMOS record all agree. Losing any one of them is not a
// build failure: it is a switch that waits at loader> in a rack.
func TestN3172TQBootsUnattendedByConstruction(t *testing.T) {
	b, script := rearmScript(t)
	if b.PartTable() != "dos" {
		t.Errorf("partition_table = %q; the loader finds its menu only on a DOS table", b.PartTable())
	}
	if b.BootRearm == "" {
		t.Fatal("no boot_rearm: BdsDxe consumes the shell request, so nothing re-arms it")
	}
	if !strings.Contains(script, "exit 0") {
		t.Error("boot-rearm must always exit 0; it runs before the datapath")
	}
}

func TestBootRearmIsValidShell(t *testing.T) {
	_, script := rearmScript(t)
	cmd := exec.Command("sh", "-n")
	cmd.Stdin = strings.NewReader(script)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("not valid shell: %v\n%s", err, out)
	}
}

// BdsDxe throws the record away unless byte 0 is the complement of the sum of
// bytes 1..19. The script writes the record and states its checksum as a
// decimal literal, so check the two agree.
func TestBootRearmChecksum(t *testing.T) {
	_, script := rearmScript(t)
	rec := map[int]int{}
	for _, m := range regexp.MustCompile(`(?m)^wr (\d+) (\d+)$`).FindAllStringSubmatch(script, -1) {
		var idx, val int
		if _, err := fmt.Sscan(m[1], &idx); err != nil {
			t.Fatal(err)
		}
		if _, err := fmt.Sscan(m[2], &val); err != nil {
			t.Fatal(err)
		}
		rec[idx-200] = val
	}
	sum := 0
	for i := 1; i < 20; i++ {
		sum += rec[i]
	}
	if want := ^sum & 0xff; rec[0] != want {
		t.Errorf("record byte 0 = %#x, want %#x", rec[0], want)
	}
	if rec[2] != 0x0a || rec[3] != 0x0b {
		t.Errorf("shell magic = %#x %#x, want 0x0a 0x0b", rec[2], rec[3])
	}
	if rec[9]&3 != 3 {
		t.Errorf("boot mode = %d; the loader only exits on a failed autoboot in mode 3", rec[9]&3)
	}
}
