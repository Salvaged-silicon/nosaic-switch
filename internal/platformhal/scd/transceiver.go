package scd

import (
	"fmt"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// The SCD's per-cage transceiver control table.
//
// The laser is gated by the SCD, not by the switch chip, so this is readable
// with the ASIC dark and is independent of anything the datapath believes. That
// independence is the point: it gives a cage numbering that owes nothing to the
// port map, which is what makes it usable for establishing one.
//
// One entry per front-panel cage, in order, the SFP+ ones first. WHERE the
// table is and HOW MANY entries it has come from the board -- see
// platformhal.CageTable -- because both differ between two boards with the
// same FPGA, and using one board's numbers on the other writes over registers
// that belong to something else.
const (
	// xcvrTXDisable is bit 6. Asserted for an empty cage and for a module the
	// board has not qualified, deasserted once one has.
	xcvrTXDisable = 1 << 6

	// xcvrQSFPLowPower is bits 5 and 7: a QSFP module's low-power and reset
	// controls. They have no counterpart on an SFP+ cage, which is why the
	// SFP+ ports came up with only TX_DISABLE cleared and the QSFP ports did
	// not come up at all.
	//
	// A module held here answers nothing. Its EEPROM does not respond and its
	// transmitter is dark, so the neighbour sits at NO-CARRIER while this end
	// reports a port that is enabled, at the right speed, with the right lane
	// map and polarity, and no error anywhere. Established on this board by
	// clearing bit 7 on the two cabled cages and watching both 40G links come
	// up at once, and before that on a 7050TX-64, where reading the same block
	// under EOS and under our own OS was what located it.
	xcvrQSFPLowPower = (1 << 5) | (1 << 7)

	// xcvrModSel is bit 0: module select, and it must be ASSERTED for the
	// module to be selected and powered.
	//
	// ⚠ CLEARING BITS IS NOT ENOUGH. This driver enabled a cage by clearing
	// TX_DISABLE and the low-power/reset pair and leaving every other bit as
	// found, which works only for as long as something else has already
	// asserted this one. The vendor writes the whole word: 0x101 on the
	// 7050TX-64, established by reading the block under the vendor OS with
	// the link up and under ours with it down.
	//
	// It does not read back as written -- the word mixes read-only status
	// with control -- so a cage written 0x101 reads 0x108, and a cage whose
	// module select was never asserted reads 0x100. That one bit of
	// difference is invisible unless a working cage is there to compare
	// against, and it is the difference between a module that answers and a
	// module that does not.
	xcvrModSel = 1 << 0
)

// cageTable is the board's cage table, or an error naming what is missing.
//
// No default: the alternative is writing a guessed register table on an FPGA
// that owns the reset lines, which is how this board was driven before and is
// exactly what the board data exists to stop.
func (s *SCD) cageTable() (*platformhal.CageTable, error) {
	if s.cages == nil {
		return nil, fmt.Errorf("%w: this board does not state its transceiver "+
			"cage table (platform_hal.cages in board.yml)", platformhal.ErrUnsupported)
	}
	return s.cages, nil
}

// Whole cage words measured on this board, kept as the evidence the decode
// below is checked against rather than as the decode itself.
//
// The first three were sampled while the vendor OS was driving the board
// controller. The last two were measured under NOSaic, which leaves the
// controller in a different state -- which is why matching whole words
// classified every cage on this switch as "undetermined".
const (
	xcvrEOSEmpty       = 0x00000047 // vendor OS, no module
	xcvrEOSPresentOff  = 0x000001c0 // vendor OS, module, laser off
	xcvrEOSPresentOn   = 0x00000180 // vendor OS, module, laser on
	xcvrEmptyNOSaic    = 0x00000187 // NOSaic, no module -- 48 cages
	xcvrPresentPreRead = 0x00000189 // NOSaic, module, before the bus is driven

	// xcvrUnconfigured is a cage nothing has powered. A module in one is
	// invisible -- it answers no EEPROM and reports no presence -- so this
	// word must never be decoded as "empty", which is what it would look
	// like to the bit decode below.
	//
	// It is matched as a whole word on purpose. There is exactly one sample
	// of it, and inferring which bit means "unconfigured" from one sample is
	// the mistake this file has already made once.
	xcvrUnconfigured = 0x000001df
)

// xcvrAbsent is bits 1 and 2: set when the cage is empty, clear when a module
// is in it.
//
// ⚠ THIS FILE USED TO MATCH WHOLE WORDS, AND THE COMMENT WHERE IT DID SAID
// WHY: an earlier version derived a bit decode from three samples, guessed
// wrong, and reported every cage on the box as populated. That warning was
// right about the method and this is not a repeat of it.
//
// The difference is ground truth. A module's EEPROM answers on the cage's own
// SMBus channel whether or not the board controller's word can be read, so
// presence can be established per cage without reference to this register at
// all. Every one of the 52 cages was probed that way and correlated against
// its word: four cages hold modules and read 0x180, forty-eight are empty and
// read 0x187, with no exceptions. Both bits agree across all five whole words
// above, taken under two different operating systems.
//
// So this is a decode measured against an independent signal on every cage on
// the board, not one inferred from a handful of samples. If it is ever wrong,
// the EEPROM sweep is how to show that.
const xcvrAbsent = 0x6

// Presence is what the cage word says about a module being there.
type Presence int

const (
	// PresenceUnknown is a cage word that is not one of the values measured
	// on this board. It is a distinct answer from "empty", and reporting it
	// as one would be inventing data.
	PresenceUnknown Presence = iota
	// PresenceUnconfigured is a cage nothing has powered. Distinct from
	// empty: a module in one of these is simply not visible, so the honest
	// answer is that presence is unreadable, not that the cage is bare.
	PresenceUnconfigured
	PresenceEmpty
	PresentLaserOff
	PresentLaserOn
)

func (p Presence) String() string {
	switch p {
	case PresenceUnconfigured:
		return "cage not powered"
	case PresenceEmpty:
		return "empty"
	case PresentLaserOff:
		return "module present, laser off"
	case PresentLaserOn:
		return "module present, laser on"
	}
	return "undetermined"
}

// Cage is one front-panel transceiver cage.
type Cage struct {
	// Index is the front-panel position, 1-based: 1..48 are the SFP+ cages
	// and 49..54 the QSFP+ ones.
	Index int
	Kind  string
	Raw   uint32
	// State is what the word says, or PresenceUnknown if it says nothing this
	// driver can read.
	State Presence
}

// Transceivers reads the cage table.
//
// This is what tells "no link because nothing is plugged in" apart from "no
// link because the port map does not reach this cage" -- two states that are
// identical from the switch chip's side and need completely different work.
func (s *SCD) Transceivers() ([]Cage, error) {
	t, err := s.cageTable()
	if err != nil {
		return nil, err
	}
	end := t.Base + t.Count*t.Stride
	if end > len(s.bar) {
		return nil, fmt.Errorf("the cage table ends at %#x, past the %d-byte BAR",
			end, len(s.bar))
	}

	cages := make([]Cage, 0, t.Count)
	for i := 0; i < t.Count; i++ {
		v := s.read32(t.Base + i*t.Stride)
		c := Cage{Index: i + 1, Raw: v, Kind: "SFP+"}
		if i >= t.SFPCount {
			c.Kind = "QSFP+"
		}
		// Presence from the absent bits, and the laser from the bit that
		// gates it. Both are established against the module EEPROMs -- see
		// xcvrAbsent -- rather than by matching the word as a whole, which
		// only ever worked on a board the vendor OS had just been driving.
		switch {
		case v == xcvrUnconfigured:
			c.State = PresenceUnconfigured
		case v&xcvrAbsent != 0:
			c.State = PresenceEmpty
		case v&xcvrTXDisable != 0:
			c.State = PresentLaserOff
		default:
			c.State = PresentLaserOn
		}
		cages = append(cages, c)
	}
	return cages, nil
}

// TXEnabled reports whether a cage's transmitter is turned on.
func (c Cage) TXEnabled() bool { return c.Raw&xcvrTXDisable == 0 }

// SetTX turns a cage's transmitter on or off.
//
// The laser is gated by the board controller, not by the switch chip, so no
// amount of correct datapath configuration lights it. On a board the vendor OS
// has not run, every SFP+ cage here reads 0xff -- TX_DISABLE asserted -- and
// the result is a link that looks healthy from this end and does not exist
// from the other: we lock onto the neighbour's light, the neighbour sees no
// carrier at all.
//
// On an SFP+ cage only bit 6 is touched: the rest of the word means things this
// driver has not established, and clearing bits whose purpose is unknown on the
// device that gates the lasers is not a trade worth making.
//
// A QSFP cage additionally needs its low-power and reset controls cleared, or
// the module never powers up. That is not optional and it is not visible from
// the datapath -- see xcvrQSFPLowPower.
//
// THE REGISTER DOES NOT READ BACK WHAT IS WRITTEN. It mixes read-only status
// with control, so 0x01 written reads 0x08. Judge it by effect rather than by
// read-back, which is why the check below tests only the bit whose behaviour is
// established.
func (s *SCD) SetTX(cage int, on bool) (before, after uint32, err error) {
	t, err := s.cageTable()
	if err != nil {
		return 0, 0, err
	}
	if cage < 1 || cage > t.Count {
		return 0, 0, fmt.Errorf("cage %d is outside 1..%d", cage, t.Count)
	}
	off := t.Base + (cage-1)*t.Stride
	if off+4 > len(s.bar) {
		return 0, 0, fmt.Errorf("cage %d is past the mapped BAR", cage)
	}

	before = s.read32(off)
	v := before | xcvrTXDisable
	if on {
		v = before &^ uint32(xcvrTXDisable)
		if cage > t.SFPCount {
			v &^= uint32(xcvrQSFPLowPower)
			v |= xcvrModSel
		}
	}
	s.write32(off, v)
	after = s.read32(off)

	// Only TX_DISABLE is checked. The low-power bits do not read back, so
	// asserting anything about them here would be asserting something false.
	if (after&xcvrTXDisable == 0) != on {
		return before, after, fmt.Errorf(
			"cage %d transmitter did not change: %#08x -> %#08x, TX_DISABLE still %v",
			cage, before, after, after&xcvrTXDisable != 0)
	}
	return before, after, nil
}
