package main

import (
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal/scd"
)

// The cooling curve.
//
// At or below tempMin the fans sit at the floor; at or above tempMax they are
// flat out; between, linear. Ported from EdgeNOS, which established these on
// this hardware.
const (
	tempMinC = 35
	tempMaxC = 65
	maxPct   = 100

	defaultInterval = 10 * time.Second

	// slewDownMax is how much the duty may fall in one interval. Rises are
	// applied immediately.
	slewDownMax = 5
)

// thermalTarget maps the hottest sensor to a fan duty.
//
// A temperature of -1 means nothing could be read, and the answer to that is
// full cooling. The failure mode of a cooling loop must be too much cooling.
func thermalTarget(hot int) int {
	switch {
	case hot < 0:
		return maxPct
	case hot <= tempMinC:
		return scd.FanFloorPercent
	case hot >= tempMaxC:
		return maxPct
	}
	return scd.FanFloorPercent +
		(hot-tempMinC)*(maxPct-scd.FanFloorPercent)/(tempMaxC-tempMinC)
}

type fanController interface {
	Temperatures() (map[string]int, error)
	SetAllFansPercent(int) (int, error)
}

// hottest returns the highest sensor reading in whole degrees, or -1 if
// nothing could be read.
func hottest(hal fanController) (int, map[string]int) {
	temps, err := hal.Temperatures()
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

func thermalCmd(hal platformhal.HAL, args []string) error {
	fc, ok := hal.(fanController)
	if !ok {
		return fmt.Errorf("%w: this board has no fan control", platformhal.ErrUnsupported)
	}

	interval, once := defaultInterval, false
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--once":
			once = true
		case "--interval":
			if i+1 >= len(args) {
				return fmt.Errorf("--interval needs a number of seconds")
			}
			i++
			n, err := strconv.Atoi(args[i])
			if err != nil || n < 1 {
				return fmt.Errorf("%q is not a number of seconds", args[i])
			}
			interval = time.Duration(n) * time.Second
		default:
			return fmt.Errorf("unknown option %q", args[i])
		}
	}

	// Start flat out and come down. Starting low and ramping up would leave
	// the box under-cooled for a whole interval if it is already hot.
	cur := maxPct
	if failed, err := fc.SetAllFansPercent(cur); err != nil {
		return fmt.Errorf("cannot command the fans at all (%d of %d refused): %w",
			failed, scd.FanCount(), err)
	}

	// However this ends -- a signal, the surrounding script dying, a panic --
	// the fans must not be left at whatever low value a cool moment happened
	// to command.
	safe := func() {
		fmt.Printf("thermal: stopping, setting fans to %d%%\n", maxPct)
		if failed, err := fc.SetAllFansPercent(maxPct); err != nil {
			fmt.Fprintf(os.Stderr,
				"thermal: COULD NOT RESTORE FULL COOLING (%d of %d refused): %v\n",
				failed, scd.FanCount(), err)
		}
	}
	defer safe()

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)

	fmt.Printf("thermal: floor %d%%, %d°C->%d°C maps %d%%->%d%%, every %s\n",
		scd.FanFloorPercent, tempMinC, tempMaxC, scd.FanFloorPercent, maxPct, interval)

	for {
		hot, temps := hottest(fc)
		want := thermalTarget(hot)

		// Up immediately, down slowly. Falling instantly makes the fans
		// oscillate around a threshold; rising slowly would be a thermal risk,
		// so the asymmetry is deliberate.
		if want > cur {
			cur = want
		} else if cur-want > slewDownMax {
			cur -= slewDownMax
		} else {
			cur = want
		}

		failed, err := fc.SetAllFansPercent(cur)
		note := ""
		if err != nil {
			// The hardware holds its last value, which is why this starts
			// high. Say so loudly and keep trying.
			note = fmt.Sprintf("   *** %d of %d FANS REFUSED ***", failed, scd.FanCount())
		}
		if hot < 0 {
			fmt.Printf("thermal: NO SENSOR READABLE -> %d%%%s\n", cur, note)
		} else {
			fmt.Printf("thermal: hottest %d°C %v -> %d%%%s\n", hot, temps, cur, note)
		}

		if once {
			return nil
		}
		select {
		case <-stop:
			return nil
		case <-time.After(interval):
		}
	}
}
