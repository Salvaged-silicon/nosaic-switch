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

// Raw returns the watchdog register verbatim.
//
// The decoded view depends on a timeout model that is measured rather than
// documented -- Arista's GPL driver puts the timeout somewhere this SCD
// revision does not -- so the raw value is what any check of that model has to
// start from.
func (w *watchdog) Raw() uint32 { return w.s.read32(watchdogReg) }

func (w *watchdog) Armed() (bool, int, error) {
	v := w.s.read32(watchdogReg)
	return v&wdEnable != 0, int(v&wdTimeoutMask) * wdTimeoutUnitMS, nil
}

// Arm starts the watchdog with a timeout in milliseconds.
//
// The value is read back and checked. Arista's own tooling warns that a disarm
// whose output was redirected away is not evidence it happened, and the same
// applies to arming: a caller that believes it is protected and is not is
// worse off than one that knows it is not.
func (w *watchdog) Arm(timeoutMS int) error {
	maxMS := wdTimeoutMask * wdTimeoutUnitMS
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
	count := uint32(timeoutMS / wdTimeoutUnitMS)

	// Bits [28:16] are preserved. Aboot leaves 500 there, its meaning is not
	// known, and clearing an unknown field on the device that power-cycles the
	// board is not a good trade for tidiness.
	high := w.s.read32(watchdogReg) & wdHighMask
	w.s.write32(watchdogReg, wdEnable|wdActionPowerCycle|count|high)

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

// WriteRaw writes the watchdog register verbatim and returns what it reads
// back.
//
// This exists because the timeout encoding on this SCD revision is not settled.
// Arista's GPL driver puts the timeout in the low 16 bits; the board notes for
// this switch concluded it lives in bits [28:16] in units of 100 ms, from an
// experiment that only ever changed the enable bit -- which cannot separate
// the two, because both fields kept the values Aboot left. Establishing it
// needs the fields varied independently, and that needs this.
//
// It is a bring-up primitive, not part of the Watchdog contract. Arm is what
// callers should use, once Arm is known to be right.
func (w *watchdog) WriteRaw(v uint32) uint32 {
	w.s.write32(watchdogReg, v)
	return w.s.read32(watchdogReg)
}
