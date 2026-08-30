package scd

import "fmt"

// The SCD's per-cage transceiver control table.
//
// The laser is gated by the SCD, not by the switch chip, so this is readable
// with the ASIC dark and is independent of anything the datapath believes. That
// independence is the point: it gives a cage numbering that owes nothing to the
// port map, which is what makes it usable for establishing one.
//
// 54 entries, 48 SFP+ cages then 6 QSFP+ cages, matching the front panel in
// order.
const (
	xcvrBase   = 0xa010
	xcvrStride = 0x10
	xcvrCount  = 54
	// xcvrSFPCount is how many of those are SFP+; the rest are QSFP+.
	xcvrSFPCount = 48

	// xcvrTXDisable is bit 6. Asserted for an empty cage and for a module the
	// board has not qualified, deasserted once one has.
	xcvrTXDisable = 1 << 6
)

// Known values of a cage word, measured on this board.
const (
	xcvrEmpty      = 0x00000047
	xcvrPresentOff = 0x000001c0
	xcvrPresentOn  = 0x00000180
)

// Presence is what the cage word says about a module being there.
type Presence int

const (
	// PresenceUnknown is a cage word that is not one of the values measured
	// on this board. It is a distinct answer from "empty", and reporting it
	// as one would be inventing data.
	PresenceUnknown Presence = iota
	PresenceEmpty
	PresentLaserOff
	PresentLaserOn
)

func (p Presence) String() string {
	switch p {
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
	end := xcvrBase + xcvrCount*xcvrStride
	if end > len(s.bar) {
		return nil, fmt.Errorf("the cage table ends at %#x, past the %d-byte BAR",
			end, len(s.bar))
	}

	cages := make([]Cage, 0, xcvrCount)
	for i := 0; i < xcvrCount; i++ {
		v := s.read32(xcvrBase + i*xcvrStride)
		c := Cage{Index: i + 1, Raw: v, Kind: "SFP+"}
		if i >= xcvrSFPCount {
			c.Kind = "QSFP+"
		}
		// Only the three words measured on this board are decoded. Anything
		// else is reported as undetermined rather than forced into one of
		// them: the meanings here come from whole-word samples taken while the
		// vendor OS was driving the SCD, and deriving a bit decode from three
		// samples in order to classify a fourth would be a guess presented as
		// a reading. An earlier version did exactly that and reported every
		// cage on the box as populated.
		switch v {
		case xcvrEmpty:
			c.State = PresenceEmpty
		case xcvrPresentOff:
			c.State = PresentLaserOff
		case xcvrPresentOn:
			c.State = PresentLaserOn
		default:
			c.State = PresenceUnknown
		}
		cages = append(cages, c)
	}
	return cages, nil
}

// Known reports whether a cage word is one of the values measured on this
// board.
func (c Cage) Known() bool { return c.State != PresenceUnknown }

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
// Only bit 6 is touched. The rest of the word means things this driver has not
// established, and clearing bits whose purpose is unknown on the device that
// gates the lasers is not a trade worth making.
func (s *SCD) SetTX(cage int, on bool) (before, after uint32, err error) {
	if cage < 1 || cage > xcvrCount {
		return 0, 0, fmt.Errorf("cage %d is outside 1..%d", cage, xcvrCount)
	}
	off := xcvrBase + (cage-1)*xcvrStride
	if off+4 > len(s.bar) {
		return 0, 0, fmt.Errorf("cage %d is past the mapped BAR", cage)
	}

	before = s.read32(off)
	v := before | xcvrTXDisable
	if on {
		v = before &^ uint32(xcvrTXDisable)
	}
	s.write32(off, v)
	after = s.read32(off)

	if (after&xcvrTXDisable == 0) != on {
		return before, after, fmt.Errorf(
			"cage %d transmitter did not change: %#08x -> %#08x, TX_DISABLE still %v",
			cage, before, after, after&xcvrTXDisable != 0)
	}
	return before, after, nil
}
