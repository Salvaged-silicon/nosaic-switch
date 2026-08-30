package scd

import (
	"fmt"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// The SMBus master is in its own GPL-2.0 package. Everything here stays
// Apache-2.0: what this file uses is a register map, which carries no licence.

// Thermal sensors and the fan controller, both behind the SCD's SMBus.
//
// Neither is on the PCI bus and neither is on the CPU's own i2c controller,
// which is why /dev/i2c-* exists on this board and shows nothing useful: the
// PIIX4 adapter is bound and these devices are somewhere else entirely.
//
// Device placement is from the board's own description, by way of EdgeNOS:
// nine accelerators at 0x8000 stride 0x80, each with numbered buses.
const (
	// lm73 is the front-panel temperature sensor.
	lm73Accel, lm73Bus, lm73Addr = 0, 0, 0x4a
	// max6658 is the board sensor.
	max6658Accel, max6658Bus, max6658Addr = 1, 0, 0x4c

	// The fan controller is a CPLD, not a fan chip: four fans, on the CPU
	// card's bus. Two earlier attempts to find its PWM register by sweeping
	// the CPLD powered this switch off, which is why the map below comes from
	// Arista's published crow-fan-driver.c rather than from probing.
	crowAccel, crowBus, crowAddr = 0, 0, 0x60
	crowFanCount                 = 4
	crowPresentReg               = 0x21
	crowRevReg                   = 0x40
)

func crowTachReg(n int) int { return n * 2 } // low byte, high byte follows
func crowPWMReg(n int) int  { return 0x10 + n }
func crowIDReg(n int) int   { return 0x18 + n }

// Temperatures reports every thermal sensor, in millidegrees Celsius.
//
// A sensor that cannot be read is reported as an error rather than omitted or
// defaulted: a thermal reading that silently goes missing on a box whose
// thermal failure mode is silent is worse than no thermal support at all.
func (s *SCD) Temperatures() (map[string]int, error) {
	out := map[string]int{}
	var firstErr error
	note := func(err error) {
		if firstErr == nil {
			firstErr = err
		}
	}

	// The MAX6658's local temperature is one byte of whole degrees.
	if v, err := s.smb().ReadReg(max6658Accel, max6658Bus, max6658Addr, 0x00); err != nil {
		note(fmt.Errorf("board sensor (MAX6658 %#02x): %w", max6658Addr, err))
	} else {
		out["board"] = int(int8(v)) * 1000
	}

	// The LM73's temperature is 16 bits; the high byte is whole degrees, which
	// is the resolution anything here acts on.
	if v, err := s.smb().ReadReg(lm73Accel, lm73Bus, lm73Addr, 0x00); err != nil {
		note(fmt.Errorf("front-panel sensor (LM73 %#02x): %w", lm73Addr, err))
	} else {
		out["front-panel"] = int(int8(v)) * 1000
	}

	if len(out) == 0 {
		return out, firstErr
	}
	return out, nil
}

// Fans reports the fan trays: presence, speed and commanded duty.
//
// Read-only. Driving them is deliberately not implemented here -- see the note
// on SetFanPWM.
func (s *SCD) Fans() ([]platformhal.Fan, error) {
	present, err := s.smb().ReadReg(crowAccel, crowBus, crowAddr, crowPresentReg)
	if err != nil {
		return nil, fmt.Errorf("fan controller (CPLD %#02x): %w", crowAddr, err)
	}

	fans := make([]platformhal.Fan, 0, crowFanCount)
	for i := 0; i < crowFanCount; i++ {
		// PRESENCE IS ACTIVE LOW: a set bit means the tray is missing. Reading
		// it the other way round reports every fan absent on a switch that is
		// running perfectly well, which is what this did first.
		f := platformhal.Fan{Index: i + 1, Present: present&(1<<uint(i)) == 0}
		lo, err1 := s.smb().ReadReg(crowAccel, crowBus, crowAddr, crowTachReg(i))
		hi, err2 := s.smb().ReadReg(crowAccel, crowBus, crowAddr, crowTachReg(i)+1)
		tach := 0
		if err1 == nil && err2 == nil {
			tach = int(lo) | int(hi)<<8
			// RPM = 6000000 / tach, from the published driver. A tach of zero
			// is no pulses at all -- a stopped fan -- and dividing by it, or
			// by the driver's substituted 1, reports six million rpm for a fan
			// that is not turning. Zero is the honest answer.
			if tach > 0 && tach != 0xffff {
				f.RPM = 6000000 / tach
			}
		}
		if v, err := s.smb().ReadReg(crowAccel, crowBus, crowAddr, crowPWMReg(i)); err == nil {
			f.Percent = int(v) * 100 / 255
		}
		f.Raw = fmt.Sprintf("presence %#02x, tach %d", present, tach)
		fans = append(fans, f)
	}
	return fans, nil
}

// FanControllerRevision identifies the CPLD, which is the cheapest check that
// the SMBus path reaches it at all.
func (s *SCD) FanControllerRevision() (byte, error) {
	return s.smb().ReadReg(crowAccel, crowBus, crowAddr, crowRevReg)
}

// FanFloorPercent is the lowest duty this driver will command.
//
// The vendor's own thermal control never goes below this and neither does
// this. A fan controller that can be told to stop is a fan controller that
// will eventually be told to stop by a bug.
const FanFloorPercent = 30

// SetFanPercent commands one fan's duty, as a percentage.
//
// The ONLY register this writes is the fan's own PWM at 0x10+n, taken from
// Arista's published driver. That restraint is not fastidiousness: two earlier
// attempts to find this register by sweeping the CPLD powered the switch off.
//
// Anything below the floor is clamped rather than obeyed, and the clamp is
// reported so a caller asking for something impossible learns that it did not
// get it.
func (s *SCD) setOneFanPercent(fan, pct int) (clamped bool, err error) {
	if fan < 0 || fan >= crowFanCount {
		return false, fmt.Errorf("fan %d is outside 0..%d", fan, crowFanCount-1)
	}
	if pct > 100 {
		pct = 100
	}
	if pct < FanFloorPercent {
		pct, clamped = FanFloorPercent, true
	}
	duty := byte(pct * 255 / 100)
	if err := s.smb().WriteReg(crowAccel, crowBus, crowAddr, crowPWMReg(fan), duty); err != nil {
		return clamped, fmt.Errorf("setting fan %d to %d%%: %w", fan+1, pct, err)
	}
	return clamped, nil
}

// FanFloorPercent is the lowest duty this board will command.
func (s *SCD) FanFloorPercent() int { return FanFloorPercent }

// FanCount is how many fan bays this board has.
func (s *SCD) FanCount() int { return crowFanCount }

// SetFanPercent commands every fan, returning how many refused.
func (s *SCD) SetFanPercent(pct int) (failed int, err error) {
	var first error
	for i := 0; i < crowFanCount; i++ {
		if _, e := s.setOneFanPercent(i, pct); e != nil {
			failed++
			if first == nil {
				first = e
			}
		}
	}
	return failed, first
}
