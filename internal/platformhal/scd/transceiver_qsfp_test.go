package scd

import "testing"

// A QSFP cage must come out of low power and reset, not merely have its
// transmitter enabled. Holding those leaves the module unpowered: its EEPROM
// does not answer and its laser is dark, so the neighbour reports NO-CARRIER
// while this end sees an enabled port at the right speed with no error
// anywhere. That is what kept both 40G links down on this board.
func TestEnablingAQSFPCageClearsLowPowerAndReset(t *testing.T) {
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
			s := &SCD{bar: make([]byte, 0x10000)}
			off := xcvrBase + (tc.cage-1)*xcvrStride
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
			if tc.cage <= xcvrSFPCount {
				keep := tc.before &^ uint32(xcvrTXDisable)
				if got != keep {
					t.Errorf("cage %d: want %#x, got %#x -- bits changed beyond TX_DISABLE",
						tc.cage, keep, got)
				}
			}
		})
	}
}
