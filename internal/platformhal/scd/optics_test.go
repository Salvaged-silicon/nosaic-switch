package scd

import (
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// The two boards in the tree, as their board.yml states them. Both are here
// because the bug this table exists to prevent is using one board's cage
// numbers on the other -- so a test with only one board in it could not have
// caught it.
var (
	sx2Cages = &platformhal.CageTable{
		Base: 0xa010, Stride: 0x10, Count: 54, SFPCount: 48,
		SFPEEPROM:  &platformhal.CageEEPROM{AccelBase: 2, BusesPerAccel: 8},
		QSFPEEPROM: &platformhal.CageEEPROM{AccelBase: 8, BusesPerAccel: 8},
	}
	tx64Cages = &platformhal.CageTable{
		Base: 0xa100, Stride: 0x10, Count: 4, SFPCount: 0,
		QSFPEEPROM: &platformhal.CageEEPROM{AccelBase: 1, BusesPerAccel: 8},
	}
)

// The cage-to-SMBus map is the part that can be silently wrong: a mistake here
// reads a different cage's module and reports its light levels under the wrong
// port number, which is worse than an error. These are the coordinates from
// each board's FDL.
func TestCageSMBusMatchesTheBoardDescription(t *testing.T) {
	for _, tc := range []struct {
		board            string
		cages            *platformhal.CageTable
		cage, accel, bus int
	}{
		// 7050SX2-72Q. Cage 1 is also the one a blind scan found
		// independently under EdgeNOS.
		{"sx2", sx2Cages, 1, 2, 0},
		{"sx2", sx2Cages, 8, 2, 7},
		{"sx2", sx2Cages, 9, 3, 0},
		{"sx2", sx2Cages, 48, 7, 7},
		{"sx2", sx2Cages, 49, 8, 0},
		{"sx2", sx2Cages, 52, 8, 3},
		{"sx2", sx2Cages, 53, 8, 4},
		{"sx2", sx2Cages, 54, 8, 5},
		// 7050TX-64: four QSFP cages on accelerator 1, buses 0..3. Its 48
		// copper ports are RJ45 and have no cage at all, which is why cage 1
		// here is a QSFP and cage 1 there is an SFP+.
		{"tx64", tx64Cages, 1, 1, 0},
		{"tx64", tx64Cages, 2, 1, 1},
		{"tx64", tx64Cages, 3, 1, 2},
		{"tx64", tx64Cages, 4, 1, 3},
	} {
		a, b := tc.cages.CageSMBus(tc.cage)
		if a != tc.accel || b != tc.bus {
			t.Errorf("%s cage %d: want accel %d bus %d, got accel %d bus %d",
				tc.board, tc.cage, tc.accel, tc.bus, a, b)
		}
	}
}

// Every cage must land on exactly one (accel, bus), or two ports share an
// EEPROM and one of them is reporting the other's optic.
func TestEveryCageHasItsOwnBus(t *testing.T) {
	for name, tbl := range map[string]*platformhal.CageTable{"sx2": sx2Cages, "tx64": tx64Cages} {
		if err := tbl.Validate(); err != nil {
			t.Errorf("%s: %v", name, err)
			continue
		}
		seen := map[[2]int]int{}
		for cage := 1; cage <= tbl.Count; cage++ {
			a, b := tbl.CageSMBus(cage)
			if prev, dup := seen[[2]int{a, b}]; dup {
				t.Errorf("%s: cages %d and %d both map to accel %d bus %d",
					name, prev, cage, a, b)
			}
			seen[[2]int{a, b}] = cage
			// Accelerator 0 is the CPU card on both boards: sensors, the fan
			// CPLD, the power controllers. A cage landing there means the map
			// would have us writing page selects at a temperature sensor.
			if a < 1 {
				t.Errorf("%s: cage %d maps to accel %d, which carries no cages",
					name, cage, a)
			}
		}
		if len(seen) != tbl.Count {
			t.Errorf("%s: want %d distinct buses, got %d", name, tbl.Count, len(seen))
		}
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
