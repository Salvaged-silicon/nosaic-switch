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
