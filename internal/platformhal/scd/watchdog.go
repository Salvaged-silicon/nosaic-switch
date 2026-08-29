package scd

import (
	"fmt"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// watchdog is the SCD's hardware watchdog.
//
// Its action is a power cycle rather than a warm reset, which is what makes it
// a real recovery path: a warm reset leaves a wedged chip wedged, where a power
// cycle brings the board back through the bootloader to whatever it is
// configured to boot.
//
// It is NOT armed at power-on. Treating it as though it were is how a hung
// NOSaic image became a manual power cycle rather than an automatic recovery,
// and why Armed() exists and is worth calling before trusting it.
type watchdog struct{ s *SCD }

// Watchdog returns the board's watchdog.
func (s *SCD) Watchdog() (platformhal.Watchdog, error) { return &watchdog{s}, nil }

func (w *watchdog) Armed() (bool, int, error) {
	v := w.s.read32(watchdogReg)
	ds := int(v>>wdTimeoutShift) & wdTimeoutMax
	return v&wdEnable != 0, ds * wdTimeoutUnitMS, nil
}

// Arm starts the watchdog with a timeout in milliseconds.
//
// The value is read back and checked. Arista's own tooling warns that a disarm
// whose output was redirected away is not evidence it happened, and the same
// applies to arming: a caller that believes it is protected and is not is
// worse off than one that knows it is not.
func (w *watchdog) Arm(timeoutMS int) error {
	maxMS := wdTimeoutMax * wdTimeoutUnitMS
	if timeoutMS < wdTimeoutUnitMS || timeoutMS > maxMS {
		return fmt.Errorf("watchdog timeout %d ms is outside %d..%d ms",
			timeoutMS, wdTimeoutUnitMS, maxMS)
	}
	if timeoutMS%wdTimeoutUnitMS != 0 {
		// Rounding silently would hand back a shorter window than was asked
		// for, and the consequence of a window expiring early is the board
		// power-cycling out from under whoever is working on it.
		return fmt.Errorf("watchdog timeout %d ms is not a multiple of %d ms, "+
			"which is this register's resolution", timeoutMS, wdTimeoutUnitMS)
	}
	ds := uint32(timeoutMS / wdTimeoutUnitMS)

	// The low half is preserved. Its meaning is not known -- both Aboot and
	// EOS leave a value in it -- and clearing a field whose purpose is unknown
	// on the device that power-cycles the board is not a good trade for
	// tidiness.
	low := w.s.read32(watchdogReg) & wdLowMask
	w.s.write32(watchdogReg, wdEnable|wdActionPowerCycle|(ds<<wdTimeoutShift)|low)

	armed, got, err := w.Armed()
	if err != nil {
		return err
	}
	if !armed {
		return fmt.Errorf("the watchdog did not arm; it reads back disabled")
	}
	if got != timeoutMS {
		return fmt.Errorf("the watchdog armed with %d ms, not the %d ms asked for", got, timeoutMS)
	}
	return nil
}

// Disarm stops the watchdog. This removes the automatic recovery, so it is for
// use with a console attached and not as a matter of course.
func (w *watchdog) Disarm() error {
	v := w.s.read32(watchdogReg)
	w.s.write32(watchdogReg, v&^uint32(wdEnable))
	if armed, _, err := w.Armed(); err != nil {
		return err
	} else if armed {
		return fmt.Errorf("the watchdog did not disarm; it still reads as enabled")
	}
	return nil
}

// Pet defers the timeout by rewriting it.
func (w *watchdog) Pet() error {
	v := w.s.read32(watchdogReg)
	if v&wdEnable == 0 {
		return fmt.Errorf("the watchdog is not armed, so there is nothing to pet")
	}
	w.s.write32(watchdogReg, v)
	return nil
}
