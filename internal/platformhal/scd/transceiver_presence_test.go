// SPDX-License-Identifier: Apache-2.0

package scd

import "testing"

// TestCageWordDecode pins every cage word measured on real hardware.
//
// The three "vendor OS" words are the original samples. The two NOSaic words
// are what this switch actually reads when NOSaic is driving it, and they are
// the reason whole-word matching reported all 52 cages as undetermined: the
// board controller is left in a different state, so no populated cage and no
// empty cage matched anything the driver knew.
//
// The expectations here are not this driver's opinion. Each was established
// by reading the module's own EEPROM on the cage's SMBus channel, which
// answers independently of the board controller's word.
func TestCageWordDecode(t *testing.T) {
	cases := []struct {
		name string
		word uint32
		want Presence
	}{
		{"vendor OS, empty", xcvrEOSEmpty, PresenceEmpty},
		{"vendor OS, module, laser off", xcvrEOSPresentOff, PresentLaserOff},
		{"vendor OS, module, laser on", xcvrEOSPresentOn, PresentLaserOn},

		// Measured under NOSaic on 2026-09-26, correlated against the module
		// EEPROMs on all 52 cages: 48 empty, 4 populated, no exceptions.
		{"NOSaic, empty (48 cages)", xcvrEmptyNOSaic, PresenceEmpty},
		{"NOSaic, module, bus not yet driven", xcvrPresentPreRead, PresentLaserOn},

		// ⚠ The bit decode alone would call this "empty", which is the
		// silent under-report the whole cage story turned on: a module in an
		// unpowered cage answers nothing, so "empty" would be a wrong answer
		// presented confidently.
		{"cage nothing has powered", xcvrUnconfigured, PresenceUnconfigured},

		// An unpowered cage that does report its module. Decodes as
		// present with the laser off, which is what it is -- nothing has
		// enabled the transmitter yet.
		{"unpowered, module detected", xcvrUnconfiguredPresent, PresentLaserOff},
	}

	for _, tc := range cases {
		var got Presence
		switch {
		case tc.word == xcvrUnconfigured:
			got = PresenceUnconfigured
		case tc.word&xcvrAbsent != 0:
			got = PresenceEmpty
		case tc.word&xcvrTXDisable != 0:
			got = PresentLaserOff
		default:
			got = PresentLaserOn
		}
		if got != tc.want {
			t.Errorf("%s: word %#x decoded as %q, want %q",
				tc.name, tc.word, got, tc.want)
		}
	}
}

// TestCageAbsentBitsDisjoint is the property the decode rests on: no word
// measured on a populated cage sets an absent bit, and no word measured on an
// empty cage clears both.
//
// If a future measurement breaks this, the decode is wrong and the whole-word
// table has to come back -- so it is worth failing loudly rather than letting
// one new sample quietly widen the meaning of "present".
func TestCageAbsentBitsDisjoint(t *testing.T) {
	populated := []uint32{xcvrEOSPresentOff, xcvrEOSPresentOn, xcvrPresentPreRead,
		xcvrUnconfiguredPresent}
	empty := []uint32{xcvrEOSEmpty, xcvrEmptyNOSaic}

	for _, w := range populated {
		if w&xcvrAbsent != 0 {
			t.Errorf("word %#x came off a cage holding a module but sets an absent bit", w)
		}
	}
	for _, w := range empty {
		if w&xcvrAbsent == 0 {
			t.Errorf("word %#x came off an empty cage but sets no absent bit", w)
		}
	}
}
