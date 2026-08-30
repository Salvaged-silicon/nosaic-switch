package main

import (
	"errors"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal/scd"
)

// An unreadable sensor must mean full cooling, not the floor and not the last
// value. The failure mode of a cooling loop has to be too much cooling.
func TestUnreadableTemperatureMeansFullCooling(t *testing.T) {
	if got := thermalTarget(-1); got != maxPct {
		t.Errorf("thermalTarget(unreadable) = %d, want %d", got, maxPct)
	}
}

// The curve, at its ends and in the middle.
func TestCoolingCurve(t *testing.T) {
	for _, tc := range []struct{ c, want int }{
		{0, scd.FanFloorPercent},
		{tempMinC, scd.FanFloorPercent},
		{tempMinC + 1, scd.FanFloorPercent + (maxPct-scd.FanFloorPercent)/(tempMaxC-tempMinC)},
		{(tempMinC + tempMaxC) / 2, 65},
		{tempMaxC, maxPct},
		{tempMaxC + 50, maxPct},
	} {
		if got := thermalTarget(tc.c); got != tc.want {
			t.Errorf("thermalTarget(%d°C) = %d, want %d", tc.c, got, tc.want)
		}
	}
}

// Never below the floor, at any temperature.
func TestTargetNeverBelowFloor(t *testing.T) {
	for c := -50; c <= 120; c++ {
		if got := thermalTarget(c); got < scd.FanFloorPercent {
			t.Fatalf("thermalTarget(%d°C) = %d, below the %d%% floor",
				c, got, scd.FanFloorPercent)
		}
	}
}

type fakeFans struct {
	temp    int // millidegrees; negative means unreadable
	set     []int
	failSet bool
}

func (f *fakeFans) Temperatures() (map[string]int, error) {
	if f.temp < 0 {
		return nil, errors.New("no sensor")
	}
	return map[string]int{"board": f.temp}, nil
}

func (f *fakeFans) SetAllFansPercent(p int) (int, error) {
	f.set = append(f.set, p)
	if f.failSet {
		return scd.FanCount(), errors.New("CPLD refused")
	}
	return 0, nil
}

// The loop must start at full rather than ramping up from the floor: a box
// that is already hot would spend a whole interval under-cooled.
func TestLoopStartsAtFullCooling(t *testing.T) {
	f := &fakeFans{temp: 20000}
	if err := thermalCmd(halOf(f), []string{"--once"}); err != nil {
		t.Fatalf("thermal: %v", err)
	}
	if len(f.set) == 0 || f.set[0] != maxPct {
		t.Fatalf("first command was %v, want %d%% first", f.set, maxPct)
	}
}

// Whatever ends the loop, the fans must be left at full -- not at whatever a
// cool moment last commanded.
func TestLoopLeavesFansAtFull(t *testing.T) {
	f := &fakeFans{temp: 20000}
	if err := thermalCmd(halOf(f), []string{"--once"}); err != nil {
		t.Fatalf("thermal: %v", err)
	}
	if last := f.set[len(f.set)-1]; last != maxPct {
		t.Errorf("left the fans at %d%%, want %d%% on exit", last, maxPct)
	}
}

// A cold box still gets the floor, never zero.
func TestColdBoxGetsTheFloorNotZero(t *testing.T) {
	f := &fakeFans{temp: 5000}
	if err := thermalCmd(halOf(f), []string{"--once"}); err != nil {
		t.Fatalf("thermal: %v", err)
	}
	for _, v := range f.set {
		if v < scd.FanFloorPercent {
			t.Fatalf("commanded %d%%, below the %d%% floor: %v",
				v, scd.FanFloorPercent, f.set)
		}
	}
}
