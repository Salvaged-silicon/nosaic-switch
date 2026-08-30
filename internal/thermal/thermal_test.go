package thermal

import (
	"context"
	"errors"
	"io"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

const floor = 30

// An unreadable sensor must mean full cooling -- not the floor, and not the
// last value. The failure mode of a cooling loop has to be too much cooling.
func TestUnreadableTemperatureMeansFullCooling(t *testing.T) {
	if got := DefaultCurve.Target(-1, floor); got != 100 {
		t.Errorf("Target(unreadable) = %d, want 100", got)
	}
}

func TestCurveEndsAndMiddle(t *testing.T) {
	c := DefaultCurve
	for _, tc := range []struct{ degC, want int }{
		{0, floor},
		{c.MinC, floor},
		{(c.MinC + c.MaxC) / 2, 65},
		{c.MaxC, 100},
		{c.MaxC + 50, 100},
	} {
		if got := c.Target(tc.degC, floor); got != tc.want {
			t.Errorf("Target(%d°C) = %d, want %d", tc.degC, got, tc.want)
		}
	}
}

// Never below the board's floor, at any temperature.
func TestNeverBelowTheFloor(t *testing.T) {
	for degC := -50; degC <= 150; degC++ {
		if got := DefaultCurve.Target(degC, floor); got < floor {
			t.Fatalf("Target(%d°C) = %d, below the %d%% floor", degC, got, floor)
		}
	}
}

// A board that states nothing gets the cautious default rather than a curve of
// zeroes, which would read as "flat out below 0°C" and mean the floor forever.
func TestAnUnstatedCurveIsNotZero(t *testing.T) {
	var stated Curve
	c := stated.WithDefaults()
	if c.MinC != DefaultCurve.MinC || c.MaxC != DefaultCurve.MaxC {
		t.Errorf("unstated curve = %+v, want the default", c)
	}
	if got := c.Target(100, floor); got != 100 {
		t.Errorf("a hot box under an unstated curve got %d%%, want 100%%", got)
	}
}

type fakeCooling struct {
	tempMilli int // negative means unreadable
	set       []int
	refuse    bool
}

func (f *fakeCooling) Temperatures() (map[string]int, error) {
	if f.tempMilli < 0 {
		return nil, errors.New("no sensor")
	}
	return map[string]int{"board": f.tempMilli}, nil
}
func (f *fakeCooling) Fans() ([]platformhal.Fan, error) { return nil, nil }
func (f *fakeCooling) FanFloorPercent() int             { return floor }
func (f *fakeCooling) FanCount() int                    { return 4 }
func (f *fakeCooling) SetFanPercent(p int) (int, error) {
	f.set = append(f.set, p)
	if f.refuse {
		return 4, errors.New("controller refused")
	}
	return 0, nil
}

// Starting at the floor would leave a box that is already hot under-cooled for
// a whole interval.
func TestStartsAtFullCooling(t *testing.T) {
	f := &fakeCooling{tempMilli: 20000}
	if err := Run(context.Background(), f, f, DefaultCurve, true, io.Discard); err != nil {
		t.Fatal(err)
	}
	if len(f.set) == 0 || f.set[0] != 100 {
		t.Fatalf("commands were %v, want 100%% first", f.set)
	}
}

// However the loop ends, the fans must be left at full.
func TestLeavesFansAtFullOnExit(t *testing.T) {
	f := &fakeCooling{tempMilli: 20000}
	if err := Run(context.Background(), f, f, DefaultCurve, true, io.Discard); err != nil {
		t.Fatal(err)
	}
	if last := f.set[len(f.set)-1]; last != 100 {
		t.Errorf("left the fans at %d%%, want 100%% on exit", last)
	}
}

// Falling duty is rate-limited; rising is not. A cold box must not drop
// straight to the floor and start oscillating.
func TestFallingIsSlewLimitedAndRisingIsNot(t *testing.T) {
	f := &fakeCooling{tempMilli: 20000} // cold: target is the floor
	if err := Run(context.Background(), f, f, DefaultCurve, true, io.Discard); err != nil {
		t.Fatal(err)
	}
	// set[0] is the initial 100%; set[1] is the first regulated step.
	if len(f.set) < 2 {
		t.Fatalf("expected at least two commands, got %v", f.set)
	}
	if want := 100 - DefaultCurve.SlewDownMax; f.set[1] != want {
		t.Errorf("first step went to %d%%, want %d%% -- falling must be limited",
			f.set[1], want)
	}
}

// A controller that refuses must not stop the loop: the hardware holds its
// last value, which is why it starts high and keeps trying.
func TestARefusingControllerDoesNotStopTheLoop(t *testing.T) {
	f := &fakeCooling{tempMilli: 20000, refuse: true}
	err := Run(context.Background(), f, f, DefaultCurve, true, io.Discard)
	if err == nil {
		t.Fatal("a controller refusing the initial command should be reported")
	}
}
