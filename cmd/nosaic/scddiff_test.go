package main

import (
	"bytes"
	"strings"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// fakeSCD returns a different value each time a "moving" offset is read.
type fakeSCD struct {
	platformhal.HAL
	size   int
	moving map[int]bool
	once   map[int]int // offset -> how many more reads change
	tick   map[int]uint32
}

func (f *fakeSCD) Len() int { return f.size }

func (f *fakeSCD) Read32(off int) uint32 {
	if f.moving[off] {
		f.tick[off]++
		return f.tick[off]
	}
	if n := f.once[off]; n > 0 {
		f.once[off] = n - 1
		f.tick[off]++
		return f.tick[off]
	}
	return 0xdeadbeef
}

// A register that changes on every pass is separated from one that changed
// once, because only the first is counting.
//
// Three passes rather than two exist for exactly this: on two passes a
// register that twitched once is indistinguishable from a counter, and a
// shortlist full of twitches is not a shortlist.
func TestSCDDiffSeparatesCountersFromOneOffChanges(t *testing.T) {
	f := &fakeSCD{
		size:   0x40,
		moving: map[int]bool{0x10: true},
		once:   map[int]int{0x20: 1},
		tick:   map[int]uint32{},
	}
	var out bytes.Buffer
	if err := scdDiffTo(&out, f, []string{"0x0", "0x40"}); err != nil {
		t.Fatal(err)
	}
	got := out.String()
	if !strings.Contains(got, "0x000010") || !strings.Contains(got, "moves every pass") {
		t.Errorf("the counting register was not reported as counting:\n%s", got)
	}
	if !strings.Contains(got, "0x000020") || !strings.Contains(got, "moved once") {
		t.Errorf("the one-off change was not reported separately:\n%s", got)
	}
	if !strings.Contains(got, "1 word(s) move every pass, 1 moved once") {
		t.Errorf("wrong tally:\n%s", got)
	}
}

// A range outside the BAR is refused rather than read.
func TestSCDDiffRefusesRangesOutsideTheBAR(t *testing.T) {
	f := &fakeSCD{size: 0x40, moving: map[int]bool{}, once: map[int]int{}, tick: map[int]uint32{}}
	var out bytes.Buffer
	if err := scdDiffTo(&out, f, []string{"0x0", "0x100"}); err == nil {
		t.Error("a range past the end of the BAR should be refused, not read")
	}
}
