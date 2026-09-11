package platformhal

import (
	"fmt"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal/sff"
)

// Optics is a board that can reach its transceivers' diagnostic memory.
//
// Optional, and deliberately the smallest thing a board can implement: give me
// n bytes from a cage. Everything above it -- which pages to read, which
// standard applies, how to turn a 16-bit field into microwatts -- is the same
// on every switch and lives in ReadModule and in package sff.
//
// The split matters because the alternative is what this HAL already suffers
// from elsewhere: PSU and transceiver status reached through type assertions
// on an SCD-specific type, so the "generic" CLI path names a board package.
// A board implementing Optics names nothing of ours and is named by nothing.
type Optics interface {
	// ReadModuleBytes returns n bytes from the module in cage (1-based, in
	// front-panel order), at i2c address addr, starting at offset.
	//
	// page selects an SFF memory page first; -1 means read the always-mapped
	// lower half and select nothing. Selecting a page is a write, so a board
	// that can only read must reject any page >= 0 rather than quietly
	// returning the wrong bytes.
	ReadModuleBytes(cage, addr, page, offset, n int) ([]byte, error)

	// CageCount is how many front-panel cages the board has.
	CageCount() int
}

// i2c addresses defined by SFF-8472 and SFF-8636.
const (
	addrA0 = 0x50 // identity, and a QSFP's diagnostics
	addrA2 = 0x51 // an SFP's diagnostics
)

// ReadModule reads and decodes one transceiver.
//
// Written once for every board. It asks the module what it is before deciding
// how to read it, because the two standards put diagnostics in different
// places -- a QSFP in page 00h at 0x50, an SFP on a second i2c address
// entirely -- and reading an SFP the QSFP way returns its identity bytes
// interpreted as temperatures.
//
// identity is false when only light levels are wanted: the vendor strings cost
// a page select on a QSFP and the numbers do not.
func ReadModule(o Optics, cage int, identity bool) (sff.Module, error) {
	if cage < 1 || cage > o.CageCount() {
		return sff.Module{}, fmt.Errorf("cage %d: this board has %d", cage, o.CageCount())
	}
	id, err := o.ReadModuleBytes(cage, addrA0, -1, 0, 1)
	if err != nil {
		return sff.Module{}, fmt.Errorf("cage %d: reading the identifier: %w", cage, err)
	}
	if len(id) == 0 {
		return sff.Module{}, fmt.Errorf("cage %d: the module returned nothing", cage)
	}

	switch sff.KindOf(id[0]) {
	case sff.QSFP:
		lower, err := o.ReadModuleBytes(cage, addrA0, -1, 0, sff.QSFPLowerLen)
		if err != nil {
			return sff.Module{}, fmt.Errorf("cage %d: reading diagnostics: %w", cage, err)
		}
		var upper []byte
		if identity {
			// Best effort. A board that cannot select a page still reports
			// light levels, which is the part somebody is usually asking for.
			upper, _ = o.ReadModuleBytes(cage, addrA0, 0, 128, sff.QSFPUpperLen)
		}
		return sff.DecodeQSFP(lower, upper), nil

	case sff.SFP:
		a0, err := o.ReadModuleBytes(cage, addrA0, -1, 0, sff.SFPA0Len)
		if err != nil {
			return sff.Module{}, fmt.Errorf("cage %d: reading identity: %w", cage, err)
		}
		var a2 []byte
		if sff.SFPHasDiagnostics(a0) {
			// Only when the module says it has them. Reading 0x51 on a module
			// without a diagnostic page returns whatever the bus floats to --
			// usually 0xff, which decodes to plausible nonsense rather than to
			// an error anybody would notice.
			a2, _ = o.ReadModuleBytes(cage, addrA2, -1, 0, sff.SFPA2Len)
		}
		return sff.DecodeSFP(a0, a2), nil
	}

	return sff.Module{Identifier: id[0]}, fmt.Errorf(
		"cage %d: identifier %#02x is not a type this reads (SFF-8024)", cage, id[0])
}
