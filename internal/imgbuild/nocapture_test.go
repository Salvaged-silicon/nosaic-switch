// SPDX-License-Identifier: Apache-2.0

package imgbuild

import (
	"os"
	"os/exec"
	"regexp"
	"strings"
	"testing"
)

// dumpLine is one line of a register dump: an address and a value, nothing
// else. Captures of a running switch look like thousands of these.
var dumpLine = regexp.MustCompile(`(?m)^\s*(0x)?[0-9a-fA-F]{4,8}\s+(0x)?[0-9a-fA-F]{8}\s*$`)

// No register dump may be committed to this tree.
//
// # WHY THIS IS A TEST AND NOT A NOTE
//
// NOSaic has to be redistributable, which means it carries no vendor
// content. Register dumps captured from a switch running the vendor's OS are
// exactly the thing that would end that, and they are useful enough during
// bring-up that the temptation is real: `fm6000-probe --load` takes one and
// writes it to the chip, which is how the scheduler wall was characterised.
//
// That tool is scaffolding. The dumps it reads live outside this repository
// and must stay there. A capture that lands in the tree would not announce
// itself -- it is a text file of hex pairs, it would pass every other test,
// and it would quietly make the images unpublishable.
//
// Authoring a table is different from carrying a capture, and the difference
// is visible: our blocks compute their addresses from geometry and their
// values from named constants, so they do not look like this.
func TestNoRegisterDumpsAreCommitted(t *testing.T) {
	out, err := exec.Command("git", "-C", "../..", "ls-files").Output()
	if err != nil {
		t.Skip("not a git checkout")
	}

	// A handful of hex pairs is a documentation example. Thousands are a
	// capture. The threshold is deliberately generous.
	const suspicious = 64

	for _, f := range strings.Split(strings.TrimSpace(string(out)), "\n") {
		if f == "" {
			continue
		}
		b, err := os.ReadFile("../../" + f)
		if err != nil || len(b) == 0 {
			continue
		}
		// This file describes the pattern, so it necessarily contains it.
		if strings.HasSuffix(f, "nocapture_test.go") {
			continue
		}
		if n := len(dumpLine.FindAllIndex(b, -1)); n >= suspicious {
			t.Errorf("%s has %d lines that are just an address and a value.\n"+
				"If that is a register dump captured from a switch, it cannot live in\n"+
				"this tree: NOSaic has to be redistributable, and a capture makes its\n"+
				"images unpublishable without saying so. Author the block from its\n"+
				"geometry instead, and keep the capture outside the repository.", f, n)
		}
	}
}
