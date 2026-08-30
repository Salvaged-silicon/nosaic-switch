package scd

import "fmt"

// Power supply presence, on the SCD's general-purpose input block.
//
// Arista publishes this register in the GPL-2.0 aristanetworks/sonic tree, as
// GpioDesc("psu1_present", 0x5000, 1) on a platform of the same generation.
// Read from that map rather than derived here.
//
// ⚠ Bits 0 and 1 are the two presence bits, but WHICH SUPPLY IS WHICH is not
// established. The published descriptor names bit 1 as psu1, so the labels
// here may be the wrong way round. Both bits read set on this board, so the
// count is right and only the naming is in doubt; settling it means pulling a
// supply, which is not a remote operation. The names carry question marks
// rather than pretending -- someone acting on "psu2 is missing" would walk to
// the wrong side of the rack.
const (
	psuBase        = 0x5000
	psu1PresentBit = 1
	psu2PresentBit = 0
)

// PSUPresent reports which supplies are fitted.
//
// Presence is a GPIO rather than anything the supply says about itself, which
// is the point: it answers for a slot that is empty and for one holding a dead
// supply, where a PMBus conversation can only answer for one alive enough to
// talk.
func (s *SCD) PSUPresent() (map[string]bool, error) {
	if psuBase+4 > len(s.bar) {
		return nil, fmt.Errorf("the PSU register at %#x is past the %d-byte BAR",
			psuBase, len(s.bar))
	}
	v := s.read32(psuBase)
	return map[string]bool{
		"psu1?": v&(1<<psu1PresentBit) != 0,
		"psu2?": v&(1<<psu2PresentBit) != 0,
	}, nil
}

// PSURaw returns the register the presence bits come from, so a reading that
// looks wrong can be checked against the word it was decoded from.
func (s *SCD) PSURaw() uint32 { return s.read32(psuBase) }
