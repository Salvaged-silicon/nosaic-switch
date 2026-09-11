package scd

import "testing"

// The cage-to-SMBus map is the part that can be silently wrong: a mistake here
// reads a different cage's module and reports its light levels under the wrong
// port number, which is worse than an error. These are the coordinates from
// the board's FDL, and the whole-table check that the regular rule reproduces
// all 72 of its entries lives in the EdgeNOS project this came from.
func TestCageSMBusMatchesTheBoardDescription(t *testing.T) {
	for _, tc := range []struct{ cage, accel, bus int }{
		// The first SFP+ cage, which is also the one a blind scan found
		// independently under EdgeNOS.
		{1, 2, 0},
		{8, 2, 7},
		{9, 3, 0},
		{48, 7, 7},
		// The six QSFP cages.
		{49, 8, 0},
		{52, 8, 3},
		{53, 8, 4},
		{54, 8, 5},
	} {
		a, b := cageSMBus(tc.cage)
		if a != tc.accel || b != tc.bus {
			t.Errorf("cage %d: want accel %d bus %d, got accel %d bus %d",
				tc.cage, tc.accel, tc.bus, a, b)
		}
	}
}

// Every cage must land on exactly one (accel, bus), or two ports share an
// EEPROM and one of them is reporting the other's optic.
func TestEveryCageHasItsOwnBus(t *testing.T) {
	seen := map[[2]int]int{}
	for cage := 1; cage <= xcvrCount; cage++ {
		a, b := cageSMBus(cage)
		if prev, dup := seen[[2]int{a, b}]; dup {
			t.Errorf("cages %d and %d both map to accel %d bus %d", prev, cage, a, b)
		}
		seen[[2]int{a, b}] = cage
		// accel 0 is the CPU card and accel 1 carries sensors, PSUs and the
		// retimer. A cage landing on either means the map is wrong in a way
		// that would have us writing page selects at a temperature sensor.
		if a < 2 || a > 8 {
			t.Errorf("cage %d maps to accel %d, which carries no cages", cage, a)
		}
	}
	if len(seen) != xcvrCount {
		t.Errorf("want %d distinct buses, got %d", xcvrCount, len(seen))
	}
}

func TestReadRangeIsChecked(t *testing.T) {
	s := &SCD{bar: make([]byte, 0x10000)}
	for _, tc := range []struct {
		name         string
		cage, off, n int
	}{
		{"cage zero", 0, 0, 1},
		{"cage past the end", 55, 0, 1},
		{"past the end of a page", 52, 250, 10},
		{"negative offset", 52, -1, 1},
		{"zero bytes", 52, 0, 0},
	} {
		if _, err := s.ReadModuleBytes(tc.cage, 0x50, -1, tc.off, tc.n); err == nil {
			t.Errorf("%s: accepted", tc.name)
		}
	}
}
