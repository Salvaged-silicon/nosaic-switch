package scd

import (
	"fmt"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal/sff"
)

// Transceiver EEPROM access, the board half of platformhal.Optics.
//
// The cage-to-SMBus map is not guessed and was not found by scanning. It comes
// from the board's own FDL -- Arista's board description, which ships on the
// switch and states, per port, the SCD transceiver register, the SMBus
// accelerator and bus reaching that port's EEPROM, and the presence interrupt
// bit. Reverse-engineered under EdgeNOS and recorded in that project's
// FDL-PORTMAP-20260813.md, with three of its SMBus predictions probed on the
// hardware and all three answering.
//
// The layout is regular, and the regularity was checked against all 72 FDL
// entries rather than assumed from the first few:
//
//	cages  1..48  SFP+   accel 2..7, eight buses each, in port order
//	cages 49..54  QSFP+  accel 8, buses 0..5
//
// It agrees with the one coordinate that had been found the hard way: a blind
// scan under EdgeNOS found an EEPROM pair at accel 2 bus 0, which is cage 1.
const (
	// opticsA0Reg is the page-select byte in an SFF-8636 module, at the top of
	// the lower half where both halves can reach it.
	opticsPageSelect = 127
)

// cageSMBus is which SMBus accelerator and bus reach a cage's module.
//
// The arithmetic is the board's, not this driver's: where the SX2 spreads 48
// SFP+ cages over six accelerators eight buses at a time, the TX-64 has four
// QSFP cages on four buses of one. See platformhal.CageTable.
func (s *SCD) cageSMBus(cage int) (accel, bus int) {
	t, err := s.cageTable()
	if err != nil {
		return -1, -1
	}
	return t.CageSMBus(cage)
}

// CageCount implements platformhal.Optics.
func (s *SCD) CageCount() int {
	t, err := s.cageTable()
	if err != nil {
		return 0
	}
	return t.Count
}

// ReadModuleBytes implements platformhal.Optics.
//
// One SMBus transaction per byte, because the accelerator's read primitive is
// read-byte-data and a module's diagnostics are under a hundred bytes. Reading
// a whole 256-byte page would be worth batching; reading light levels is not.
func (s *SCD) ReadModuleBytes(cage, addr, page, offset, n int) ([]byte, error) {
	t, err := s.cageTable()
	if err != nil {
		return nil, err
	}
	if cage < 1 || cage > t.Count {
		return nil, fmt.Errorf("cage %d: this board has %d", cage, t.Count)
	}
	if n <= 0 || offset < 0 || offset+n > 256 {
		return nil, fmt.Errorf("cage %d: %d bytes at offset %d is outside a 256-byte page",
			cage, n, offset)
	}
	accel, bus := s.cageSMBus(cage)
	m := s.smb()

	if page >= 0 {
		// A page select is a write, and writing to a module that is absent or
		// wedged is the one operation here that can leave something in a state
		// it was not in before. It is only reached for the identity strings.
		if err := m.WriteReg(accel, bus, addr, opticsPageSelect, byte(page)); err != nil {
			return nil, fmt.Errorf("cage %d: selecting page %d: %w", cage, page, err)
		}
	}

	out := make([]byte, n)
	for i := 0; i < n; i++ {
		v, err := m.ReadReg(accel, bus, addr, offset+i)
		if err != nil {
			return nil, fmt.Errorf("cage %d (accel %d bus %d addr %#02x): reading byte %d: %w",
				cage, accel, bus, addr, offset+i, err)
		}
		out[i] = v
	}
	return out, nil
}

// ModuleKindHint is what the cage is wired for, independent of what is plugged
// into it. Useful when the module cannot be read at all: it says whether an
// empty QSFP cage or a failed read is being looked at.
func (s *SCD) ModuleKindHint(cage int) sff.Kind {
	t, err := s.cageTable()
	if err != nil || cage < 1 || cage > t.Count {
		return sff.Unknown
	}
	if cage > t.SFPCount {
		return sff.QSFP
	}
	return sff.SFP
}
