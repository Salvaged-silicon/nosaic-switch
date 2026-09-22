package main

import "testing"

// The real words read from a 7050SX2 on 2026-09-22, alongside the link state
// of that switch at that moment: et49, et52, et53 and et54 were carrying OSPF
// adjacencies; et50 and et51 had no module at all.
//
// This is a regression test for a mistake already made once. The first version
// of this decode indexed the bitmap by PHYSICAL port, which reported all four
// working ports as missing and would have sent someone chasing a fault that
// was not there.
func TestDecodeLinkBmap(t *testing.T) {
	words := []uint32{0xffffffff, 0x2223ffff, 0x00000022, 0x00000200}
	set := decodeLinkBmap(words)

	// The six QSFP cages, by logical port. All present, including the two with
	// no module -- the bitmap is not link-gated on this board, which is the
	// finding that retired it as an explanation for a silent port.
	for _, lg := range []int{49, 53, 57, 61, 65, 69} {
		if !set[lg] {
			t.Errorf("logical %d should be set: the QSFP bases all are", lg)
		}
	}
	// Logical 1..48 are the SFP+ front panel and are all set.
	for lg := 1; lg <= 48; lg++ {
		if !set[lg] {
			t.Errorf("logical %d (SFP+) should be set", lg)
		}
	}
	// A physical-indexed decode would have claimed these; they must NOT be set,
	// because nothing on this board uses those logical numbers.
	for _, lg := range []int{73, 81, 85, 97} {
		if set[lg] {
			t.Errorf("logical %d is set -- that is the physical numbering of a "+
				"QSFP cage, so this decode has slipped back to physical", lg)
		}
	}
	if len(set) != 56 {
		t.Errorf("want 56 bits set, got %d", len(set))
	}
}
