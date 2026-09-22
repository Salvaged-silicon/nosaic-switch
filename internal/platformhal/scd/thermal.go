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
// ⚠ WHERE THEY SIT IS THE BOARD'S TO SAY, NOT THIS FILE'S.
//
// This file used to name the accelerator, the bus and the address as
// constants, taken from the first Arista board in the tree. They are right for
// that board and wrong for the second: the 7050TX-64 puts its fan CPLD on the
// CPU card's bus, bus 1, and a write to bus 0 lands on a bus with nothing at
// 0x60. Nothing about that failure says "wrong bus" -- it says the controller
// refused the command, four times a cycle, for ever, and the fans stay wherever
// the last owner of the register left them.
//
// So placement comes from board.yml and the register maps stay here, which is
// the right split: a MAX6658 is a MAX6658 wherever it is soldered.
const (
	// The fan controller is a CPLD, not a fan chip. Two earlier attempts to
	// find its PWM register by sweeping it powered a switch off, which is why
	// the map below comes from Arista's published crow-fan-driver.c rather
	// than from probing.
	crowPresentReg = 0x21
	crowRevReg     = 0x40
)

func crowTachReg(n int) int { return n * 2 } // low byte, high byte follows
func crowPWMReg(n int) int  { return 0x10 + n }
func crowIDReg(n int) int   { return 0x18 + n }

// errNoSMBusMap is the answer when a board has not said where anything is.
//
// Deliberately not a default. A default here is a guess at a hardware address
// on a bus carrying power controllers, and the way it fails is silent.
func (s *SCD) errNoSMBusMap(what string) error {
	return fmt.Errorf("%w: this board does not state where its %s sits on the "+
		"SMBus (platform_hal.smbus in board.yml)", platformhal.ErrUnsupported, what)
}

func (s *SCD) fanController() (*platformhal.FanController, error) {
	if s.smbusMap == nil || s.smbusMap.Fans == nil {
		return nil, s.errNoSMBusMap("fan controller")
	}
	return s.smbusMap.Fans, nil
}

// Temperatures reports every thermal sensor the board declares, in
// millidegrees Celsius.
//
// A sensor that cannot be read is reported as an error rather than omitted or
// defaulted: a thermal reading that silently goes missing on a box whose
// thermal failure mode is silent is worse than no thermal support at all.
func (s *SCD) Temperatures() (map[string]int, error) {
	if s.smbusMap == nil || len(s.smbusMap.Sensors) == 0 {
		return nil, s.errNoSMBusMap("temperature sensors")
	}

	out := map[string]int{}
	var firstErr error
	for _, sensor := range s.smbusMap.Sensors {
		reg, ok := platformhal.SMBusParts[sensor.Part]
		if !ok {
			// Validation rejects this before Open, so reaching it means the
			// part table and the validator have drifted apart.
			return out, fmt.Errorf("sensor %q: unknown part %q", sensor.Name, sensor.Part)
		}
		v, err := s.smb().ReadReg(sensor.Accel, sensor.Bus, sensor.Addr, reg)
		if err != nil {
			if firstErr == nil {
				firstErr = fmt.Errorf("%s sensor (%s at a%d b%d %#02x): %w",
					sensor.Name, sensor.Part, sensor.Accel, sensor.Bus, sensor.Addr, err)
			}
			continue
		}
		// Every part here reports whole degrees, signed, in one byte: the
		// MAX6658 directly, the LM73 in the high byte of its 16-bit reading.
		// Whole degrees is the resolution anything in NOSaic acts on.
		out[sensor.Name] = int(int8(v)) * 1000
	}

	if len(out) == 0 {
		return out, firstErr
	}
	return out, nil
}

// Fans reports the fan trays: presence, speed and commanded duty.
func (s *SCD) Fans() ([]platformhal.Fan, error) {
	fc, err := s.fanController()
	if err != nil {
		return nil, err
	}
	present, err := s.smb().ReadReg(fc.Accel, fc.Bus, fc.Addr, crowPresentReg)
	if err != nil {
		return nil, fmt.Errorf("fan controller (CPLD %#02x on a%d b%d): %w",
			fc.Addr, fc.Accel, fc.Bus, err)
	}

	fans := make([]platformhal.Fan, 0, fc.Count)
	for i := 0; i < fc.Count; i++ {
		// PRESENCE IS ACTIVE LOW: a set bit means the tray is missing. Reading
		// it the other way round reports every fan absent on a switch that is
		// running perfectly well, which is what this did first.
		f := platformhal.Fan{Index: i + 1, Present: present&(1<<uint(i)) == 0}
		lo, err1 := s.smb().ReadReg(fc.Accel, fc.Bus, fc.Addr, crowTachReg(i))
		hi, err2 := s.smb().ReadReg(fc.Accel, fc.Bus, fc.Addr, crowTachReg(i)+1)
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
		// Against the board's own full scale, not against 255. Reporting a
		// tray at full speed as 70% is how a wrong scale hides.
		if v, err := s.smb().ReadReg(fc.Accel, fc.Bus, fc.Addr, crowPWMReg(i)); err == nil {
			f.Percent = int(v) * 100 / fc.MaxPWM
			if f.Percent > 100 {
				f.Percent = 100
			}
		}
		f.Raw = fmt.Sprintf("presence %#02x, tach %d", present, tach)
		fans = append(fans, f)
	}
	return fans, nil
}

// FanControllerRevision identifies the CPLD, which is the cheapest check that
// the SMBus path reaches it at all.
func (s *SCD) FanControllerRevision() (byte, error) {
	fc, err := s.fanController()
	if err != nil {
		return 0, err
	}
	return s.smb().ReadReg(fc.Accel, fc.Bus, fc.Addr, crowRevReg)
}

// SetFanPercent commands one fan's duty, as a percentage.
//
// The ONLY register this writes is the fan's own PWM at 0x10+n, taken from
// Arista's published driver. That restraint is not fastidiousness: two earlier
// attempts to find this register by sweeping the CPLD powered the switch off.
//
// Anything below the board's floor is clamped rather than obeyed, and the
// clamp is reported so a caller asking for something impossible learns that it
// did not get it.
func (s *SCD) setOneFanPercent(fan, pct int) (clamped bool, err error) {
	fc, err := s.fanController()
	if err != nil {
		return false, err
	}
	if fan < 0 || fan >= fc.Count {
		return false, fmt.Errorf("fan %d is outside 0..%d", fan, fc.Count-1)
	}
	if pct > 100 {
		pct = 100
	}
	if pct < fc.FloorPercent {
		pct, clamped = fc.FloorPercent, true
	}
	duty := byte(pct * fc.MaxPWM / 100)
	if err := s.smb().WriteReg(fc.Accel, fc.Bus, fc.Addr, crowPWMReg(fan), duty); err != nil {
		return clamped, fmt.Errorf("setting fan %d to %d%% (pwm %d of %d): %w",
			fan+1, pct, duty, fc.MaxPWM, err)
	}
	return clamped, nil
}

// FanFloorPercent is the lowest duty this board will command.
//
// A fan controller that can be told to stop is a fan controller that will
// eventually be told to stop by a bug, so the floor is the board's and it is
// enforced at the write.
func (s *SCD) FanFloorPercent() int {
	fc, err := s.fanController()
	if err != nil {
		// Nothing can be commanded anyway; the honest answer to "how slow may
		// these fans go" on a board with no fan controller is "not at all".
		return 100
	}
	return fc.FloorPercent
}

// FanCount is how many fan bays this board has.
func (s *SCD) FanCount() int {
	fc, err := s.fanController()
	if err != nil {
		return 0
	}
	return fc.Count
}

// SetFanPercent commands every fan, returning how many refused.
func (s *SCD) SetFanPercent(pct int) (failed int, err error) {
	fc, ferr := s.fanController()
	if ferr != nil {
		return 0, ferr
	}
	var first error
	for i := 0; i < fc.Count; i++ {
		if _, e := s.setOneFanPercent(i, pct); e != nil {
			failed++
			if first == nil {
				first = e
			}
		}
	}
	return failed, first
}
