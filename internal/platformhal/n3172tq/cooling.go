/* SPDX-License-Identifier: Apache-2.0 */
package n3172tq

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// Cooling is implemented only when the board names a fan controller, so a
// fanless board reports "this board has no fans" through the CLI's own check
// rather than through a method that cannot work.
//
// hwmon's pwmN is 0-255 and this interface is a percentage, so the conversion
// happens here once. It rounds up: asking for 30% and getting 29 is a board
// running very slightly cooler than requested, and the other way round is a
// board running hotter than requested, which is the direction that matters.
func (h *hal) fansDir() (string, *Fans, error) {
	f := h.d.Hwmon.Fans
	if f == nil {
		return "", nil, fmt.Errorf("%w: this board names no fan controller",
			platformhal.ErrUnsupported)
	}
	dirs, err := h.chipDirs(f.Chip)
	if err != nil || len(dirs) == 0 {
		return "", nil, fmt.Errorf("%w: no %s is bound; are this board's i2c "+
			"devices instantiated?", platformhal.ErrUnsupported, f.Chip)
	}
	return dirs[0], f, nil
}

func pwmToPercent(raw int) int { return (raw*100 + 127) / 255 }
func percentToPWM(p int) int   { return (p*255 + 99) / 100 }

// Fans reports the bays: presence, speed and commanded duty.
//
// Presence is inferred from the tachometer, because this controller has no
// presence line: a bay reading 0 RPM is either empty or has a dead fan, and
// those are the same thing to a cooling loop. It is reported as absent rather
// than as a 0 RPM fan so that "one bay is dead" is visible.
func (h *hal) Fans() ([]platformhal.Fan, error) {
	dir, f, err := h.fansDir()
	if err != nil {
		return nil, err
	}
	out := make([]platformhal.Fan, 0, f.Count)
	for i := 1; i <= f.Count; i++ {
		fan := platformhal.Fan{Index: i}
		rpm, rpmErr := readInt(filepath.Join(dir, fmt.Sprintf("fan%d_input", i)))
		if rpmErr == nil {
			fan.RPM = rpm
			fan.Present = rpm > 0
		}
		// The duty is per-controller on some parts and per-fan on others.
		// Try this fan's own pwm and fall back to the first, rather than
		// reporting 0 for a fan that is plainly spinning.
		raw, pwmErr := readInt(filepath.Join(dir, fmt.Sprintf("pwm%d", i)))
		if pwmErr != nil {
			raw, pwmErr = readInt(filepath.Join(dir, "pwm1"))
		}
		if pwmErr == nil {
			fan.Percent = pwmToPercent(raw)
			fan.Raw = fmt.Sprintf("pwm=%d rpm=%d", raw, fan.RPM)
		} else {
			fan.Raw = fmt.Sprintf("rpm=%d", fan.RPM)
		}
		out = append(out, fan)
	}
	return out, nil
}

// SetFanPercent commands every bay, clamped to the board's floor.
//
// It returns how many refused so a caller can tell "the controller is not
// listening" from "one bay is dead": those need different responses, and a
// single error cannot say which happened.
func (h *hal) SetFanPercent(percent int) (int, error) {
	dir, f, err := h.fansDir()
	if err != nil {
		return 0, err
	}
	if percent > 100 {
		percent = 100
	}
	// Clamped rather than obeyed. A controller that can be told to stop the
	// fans is one that will eventually be told to stop them by a bug.
	if percent < f.FloorPercent {
		percent = f.FloorPercent
	}
	raw := percentToPWM(percent)

	// Manual control first where the part has it: writing pwmN while the
	// controller is in automatic mode is accepted and then overridden, which
	// looks like a fan that ignores its commanded duty.
	for i := 1; i <= f.Count; i++ {
		p := filepath.Join(dir, fmt.Sprintf("pwm%d_enable", i))
		if _, err := os.Stat(p); err == nil {
			_ = os.WriteFile(p, []byte("1\n"), 0o644)
		}
	}

	refused, wrote := 0, 0
	for i := 1; i <= f.Count; i++ {
		p := filepath.Join(dir, fmt.Sprintf("pwm%d", i))
		if _, err := os.Stat(p); err != nil {
			continue // this part drives all bays from one register
		}
		if err := os.WriteFile(p, []byte(fmt.Sprintf("%d\n", raw)), 0o644); err != nil {
			refused++
			continue
		}
		wrote++
	}
	if wrote == 0 {
		// One register for the whole controller is a normal design, so try it
		// before reporting that nothing took the command.
		p := filepath.Join(dir, "pwm1")
		if err := os.WriteFile(p, []byte(fmt.Sprintf("%d\n", raw)), 0o644); err != nil {
			return f.Count, fmt.Errorf("no fan took %d%%: %w", percent, err)
		}
	}
	return refused, nil
}

func (h *hal) FanFloorPercent() int {
	if f := h.d.Hwmon.Fans; f != nil {
		return f.FloorPercent
	}
	return 0
}

func (h *hal) FanCount() int {
	if f := h.d.Hwmon.Fans; f != nil {
		return f.Count
	}
	return 0
}
