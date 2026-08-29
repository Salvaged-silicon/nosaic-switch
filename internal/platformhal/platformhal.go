// Package platformhal is the contract for a switch's own hardware, as opposed
// to its forwarding silicon.
//
// The split from switch-api is deliberate and is the one EdgeNOS got wrong:
// forwarding and box hardware are different problems with different lifetimes.
// A fan, a PSU and a reset line outlive several generations of ASIC, and a
// board that cannot report its own temperature is a different failure from one
// that cannot forward.
//
// Everything here is per-board. Unlike the datapath, where one implementation
// serves every switch with a given ASIC, this is where boards genuinely differ:
// two switches with the same silicon can have entirely different controllers
// for their fans, LEDs and resets.
package platformhal

import (
	"context"
	"errors"
)

// ErrUnsupported is returned by a board that cannot do something, rather than
// pretending to. A HAL that invents a plausible temperature makes every
// consumer of it untestable, and a switch whose thermal readings are fiction
// is worse than one that admits it has none.
var ErrUnsupported = errors.New("not supported on this board")

// Reset is a hardware reset line the board controls.
//
// The switch chip's is the one that matters at bring-up: on Arista hardware the
// board controller holds the ASIC in reset from power-on, and until it is
// released the chip does not appear on the PCI bus at all. That is a property
// of the board, not of the silicon, which is why it lives here.
type Reset string

const (
	// ResetSwitchCore is the switch chip's core logic.
	ResetSwitchCore Reset = "switch-core"
	// ResetSwitchPCIe is the switch chip's PCI Express interface. Separate
	// from the core reset, and the one that decides whether the chip is
	// enumerable.
	ResetSwitchPCIe Reset = "switch-pcie"
)

// Watchdog is the board's hardware watchdog.
//
// Arm it before anything experimental. It is the recovery path when an image
// wedges: nothing punches it, the board resets, and the bootloader brings back
// whatever it is configured to boot. It is not armed by default -- assuming it
// was is how a hung switch became a trip to the power distribution unit.
type Watchdog interface {
	// Armed reports whether the watchdog is running and its timeout.
	Armed() (armed bool, timeoutMS int, err error)
	// Arm starts it. A board that cannot returns ErrUnsupported rather than
	// silently doing nothing, because a caller that believes it is protected
	// and is not is worse off than one that knows it is not.
	Arm(timeoutMS int) error
	// Disarm stops it. Use only with a console attached: it removes the
	// automatic recovery.
	Disarm() error
	// Pet defers the timeout.
	Pet() error
}

// HAL is what a board offers. Every method may return ErrUnsupported.
type HAL interface {
	// Board identifies the hardware: model, serial, revision.
	Board() (Identity, error)

	// ResetState reports whether a line is asserted, i.e. held in reset.
	ResetState(Reset) (asserted bool, err error)
	// ReleaseSwitchChip takes the switch ASIC out of reset and waits for it
	// to appear on the bus. It is one call rather than a sequence of register
	// writes because the ordering and the delays between them are part of the
	// hardware's contract, not the caller's business to get right.
	ReleaseSwitchChip(context.Context) error

	// Watchdog returns the board's watchdog, or ErrUnsupported.
	Watchdog() (Watchdog, error)

	// Temperatures, in millidegrees Celsius, keyed by sensor name.
	Temperatures() (map[string]int, error)
	// PSUPresent reports which power supplies are fitted.
	PSUPresent() (map[string]bool, error)
}

// Identity is what a board says it is. It comes from board data -- on Arista
// hardware a prefdl structure on an i2c SEEPROM -- rather than from anything
// the OS is told at build time, so a running switch can be identified from
// itself.
type Identity struct {
	Model    string
	Serial   string
	Revision string
	// SID is the vendor's own board identifier, which is what their tooling
	// and documentation are keyed on.
	SID string
}
