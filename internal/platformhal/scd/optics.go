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
func cageSMBus(cage int) (accel, bus int) {
	if cage <= xcvrSFPCount {
		return 2 + (cage-1)/8, (cage - 1) % 8
	}
	return 8, cage - 1 - xcvrSFPCount
}

// CageCount implements platformhal.Optics.
func (s *SCD) CageCount() int { return xcvrCount }

// ReadModuleBytes implements platformhal.Optics.
//
// One SMBus transaction per byte, because the accelerator's read primitive is
// read-byte-data and a module's diagnostics are under a hundred bytes. Reading
// a whole 256-byte page would be worth batching; reading light levels is not.
func (s *SCD) ReadModuleBytes(cage, addr, page, offset, n int) ([]byte, error) {
	if cage < 1 || cage > xcvrCount {
		return nil, fmt.Errorf("cage %d: this board has %d", cage, xcvrCount)
	}
	if n <= 0 || offset < 0 || offset+n > 256 {
		return nil, fmt.Errorf("cage %d: %d bytes at offset %d is outside a 256-byte page",
			cage, n, offset)
	}
	accel, bus := cageSMBus(cage)
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
	if cage > xcvrSFPCount && cage <= xcvrCount {
		return sff.QSFP
	}
	if cage >= 1 {
		return sff.SFP
	}
	return sff.Unknown
}
