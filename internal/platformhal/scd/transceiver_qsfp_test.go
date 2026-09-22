package scd

import "testing"

// A QSFP cage must come out of low power and reset, not merely have its
// transmitter enabled. Holding those leaves the module unpowered: its EEPROM
// does not answer and its laser is dark, so the neighbour reports NO-CARRIER
// while this end sees an enabled port at the right speed with no error
// anywhere. That is what kept both 40G links down on this board.
func TestEnablingAQSFPCageClearsLowPowerAndReset(t *testing.T) {
	// Against the 7050SX2's table, where cage 53 is a QSFP and cage 2 an SFP+.
	for _, tc := range []struct {
		name       string
		cage       int
		before     uint32
		wantOffBit uint32
	}{
		{"qsfp clears low power and reset", 53, 0x81, xcvrQSFPLowPower},
		{"sfp+ leaves the rest of the word alone", 2, 0xc8, 0},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := &SCD{bar: make([]byte, 0x10000), cages: sx2Cages}
			off := sx2Cages.Base + (tc.cage-1)*sx2Cages.Stride
			s.write32(off, tc.before)

			if _, _, err := s.SetTX(tc.cage, true); err != nil {
				t.Fatalf("SetTX: %v", err)
			}
			got := s.read32(off)

			if got&xcvrTXDisable != 0 {
				t.Errorf("cage %d: TX_DISABLE still set (%#x)", tc.cage, got)
			}
			if tc.wantOffBit != 0 && got&tc.wantOffBit != 0 {
				t.Errorf("cage %d: low-power/reset still set: %#x", tc.cage, got)
			}
			// An SFP+ cage must not have bits cleared that were never ours to
			// touch -- the QSFP-only handling must not leak onto it.
			if tc.cage <= sx2Cages.SFPCount {
				keep := tc.before &^ uint32(xcvrTXDisable)
				if got != keep {
					t.Errorf("cage %d: want %#x, got %#x -- bits changed beyond TX_DISABLE",
						tc.cage, keep, got)
				}
			}
		})
	}
}

// A board that has not stated its cage table must be refused, not driven on
// another board's numbers. This is the failure the table was made data for:
// the 7050TX-64 has four cages at 0xa100 and was being driven with the SX2's
// fifty-four at 0xa010, straight across its per-cage LED block.
func TestCagesRefuseToRunWithoutABoardTable(t *testing.T) {
	s := &SCD{bar: make([]byte, 0x10000)}
	if _, _, err := s.SetTX(1, true); err == nil {
		t.Error("SetTX wrote a cage register on a board that states no cage table")
	}
	if _, err := s.Transceivers(); err == nil {
		t.Error("Transceivers read a cage table the board never stated")
	}
	if n := s.CageCount(); n != 0 {
		t.Errorf("CageCount is %d on a board with no stated cages, want 0", n)
	}
}

// The TX-64's four QSFP cages are all QSFP, so cage 1 there must get the
// low-power and reset handling that cage 1 on the SX2 must not.
func TestFirstCageIsQSFPOnABoardWithNoSFPCages(t *testing.T) {
	s := &SCD{bar: make([]byte, 0x10000), cages: tx64Cages}
	off := tx64Cages.Base
	s.write32(off, 0x81)
	if _, _, err := s.SetTX(1, true); err != nil {
		t.Fatalf("SetTX: %v", err)
	}
	got := s.read32(off)
	if got&xcvrQSFPLowPower != 0 {
		t.Errorf("cage 1: low-power/reset still set: %#x", got)
	}
	// Module select must be ASSERTED, not merely left alone: a cage whose
	// select was never driven reports a module that does not answer.
	if got&xcvrModSel == 0 {
		t.Errorf("cage 1: module select not asserted: %#x", got)
	}
	if s.ModuleKindHint(1).String() == "SFP" {
		t.Error("cage 1 is reported as SFP+ on a board whose cages are all QSFP+")
	}
}
