// Package thermal is NOSaic's cooling loop.
//
// Every switch has fans and sensors, so this is board-independent: it talks to
// platformhal.Cooling and nothing else. What varies per board is the curve --
// where a chassis wants full cooling differs with its airflow and its silicon
// -- and that is board data, not code.
//
// The design is EdgeNOS's, which ran it on the Arista 7050SX2, and the
// reasoning is preserved with it because every property here exists to stop a
// specific way of cooking a switch.
package thermal

import (
	"context"
	"fmt"
	"io"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// Curve is where a board wants its fans.
type Curve struct {
	// MinC and below sits at the floor; MaxC and above is flat out.
	MinC, MaxC int
	// SlewDownMax is how much the duty may fall in one interval. Rises are
	// applied immediately.
	SlewDownMax int
	Interval    time.Duration
}

// DefaultCurve is used where a board states nothing. Deliberately cautious:
// a board that has not said what it wants gets more cooling, not less.
var DefaultCurve = Curve{MinC: 35, MaxC: 65, SlewDownMax: 5, Interval: 10 * time.Second}

// WithDefaults fills in anything a board left unset.
func (c Curve) WithDefaults() Curve {
	if c.MinC == 0 && c.MaxC == 0 {
		c.MinC, c.MaxC = DefaultCurve.MinC, DefaultCurve.MaxC
	}
	if c.SlewDownMax <= 0 {
		c.SlewDownMax = DefaultCurve.SlewDownMax
	}
	if c.Interval <= 0 {
		c.Interval = DefaultCurve.Interval
	}
	return c
}

// Target maps the hottest sensor to a fan duty.
//
// A temperature below zero means nothing could be read, and the answer to that
// is full cooling. The failure mode of a cooling loop must be too much
// cooling; the one thing it must never do is hold its last value because it
// could not measure anything.
func (c Curve) Target(hotC, floor int) int {
	c = c.WithDefaults()
	switch {
	case hotC < 0:
		return 100
	case hotC <= c.MinC:
		return floor
	case hotC >= c.MaxC:
		return 100
	}
	return floor + (hotC-c.MinC)*(100-floor)/(c.MaxC-c.MinC)
}

// Sensors is the part of the HAL the loop reads.
type Sensors interface {
	Temperatures() (map[string]int, error)
}

// Hottest returns the highest sensor reading in whole degrees, or -1 if
// nothing could be read.
func Hottest(s Sensors) (int, map[string]int) {
	temps, err := s.Temperatures()
	if err != nil && len(temps) == 0 {
		return -1, nil
	}
	hot := -1
	for _, milli := range temps {
		if c := milli / 1000; c > hot {
			hot = c
		}
	}
	return hot, temps
}

// Run drives the fans until the context is cancelled.
//
// It starts at full and comes down: starting at the floor and ramping up
// leaves a box that is already hot under-cooled for a whole interval. However
// it ends -- cancellation, a failure, the caller dying -- the fans are left at
// full, not at whatever a cool moment happened to command.
func Run(ctx context.Context, c platformhal.Cooling, s Sensors, curve Curve, once bool, log io.Writer) error {
	curve = curve.WithDefaults()
	floor := c.FanFloorPercent()

	cur := 100
	if refused, err := c.SetFanPercent(cur); err != nil {
		return fmt.Errorf("cannot command the fans at all (%d of %d refused): %w",
			refused, c.FanCount(), err)
	}

	defer func() {
		fmt.Fprintf(log, "thermal: stopping, setting fans to 100%%\n")
		if refused, err := c.SetFanPercent(100); err != nil {
			fmt.Fprintf(log, "thermal: COULD NOT RESTORE FULL COOLING "+
				"(%d of %d refused): %v\n", refused, c.FanCount(), err)
		}
	}()

	fmt.Fprintf(log, "thermal: floor %d%%, %d°C->%d°C maps %d%%->100%%, every %s\n",
		floor, curve.MinC, curve.MaxC, floor, curve.Interval)

	for {
		hot, temps := Hottest(s)
		want := curve.Target(hot, floor)

		// Up immediately, down slowly. Falling instantly makes the fans
		// oscillate around a threshold; rising slowly would be a thermal risk,
		// so the asymmetry is deliberate.
		switch {
		case want > cur:
			cur = want
		case cur-want > curve.SlewDownMax:
			cur -= curve.SlewDownMax
		default:
			cur = want
		}

		note := ""
		if refused, err := c.SetFanPercent(cur); err != nil {
			// The hardware holds its last value, which is why this starts
			// high. Say so loudly and keep trying.
			note = fmt.Sprintf("   *** %d of %d FANS REFUSED ***", refused, c.FanCount())
		}
		if hot < 0 {
			fmt.Fprintf(log, "thermal: NO SENSOR READABLE -> %d%%%s\n", cur, note)
		} else {
			fmt.Fprintf(log, "thermal: hottest %d°C %v -> %d%%%s\n", hot, temps, cur, note)
		}

		if once {
			return nil
		}
		select {
		case <-ctx.Done():
			return nil
		case <-time.After(curve.Interval):
		}
	}
}
