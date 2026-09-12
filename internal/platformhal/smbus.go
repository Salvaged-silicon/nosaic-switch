package platformhal

import "fmt"

// Where a board's platform devices sit on its controller's SMBus.
//
// ⚠ THIS IS BOARD DATA, NOT DRIVER DATA, AND IT USED TO BE A CONSTANT.
//
// The SCD driver carried one hardcoded placement for every Arista board: the
// fan CPLD at accelerator 0, bus 0, address 0x60. That is correct for the
// 7050SX2 and wrong for the 7050TX-64, where the CPLD is on bus 1 -- the CPU
// card's bus -- and a write to bus 0 reaches an address with nothing on it.
// The result is not a diagnosable error but a healthy fan controller that
// refuses every command, on a box whose thermal failure mode is silent.
//
// The register map of a part stays in the driver, because a MAX6658 is a
// MAX6658 wherever it is soldered. Which bus it is soldered to is the board's
// to say.
type SMBusMap struct {
	Sensors []SMBusSensor  `yaml:"sensors"`
	Fans    *FanController `yaml:"fans"`
}

// SMBusAddr locates one device: which accelerator, which of its buses, and the
// 7-bit address on that bus.
type SMBusAddr struct {
	Accel int `yaml:"accel"`
	Bus   int `yaml:"bus"`
	Addr  int `yaml:"addr"`
}

// SMBusSensor is one temperature sensor.
//
// Name is what the reading is called in output, so it describes what the part
// measures rather than what the part is: two boards both carry a MAX6658 whose
// local diode is the board sensor and whose remote diode is somewhere else
// entirely.
type SMBusSensor struct {
	Name      string `yaml:"name"`
	Part      string `yaml:"part"`
	SMBusAddr `yaml:",inline"`
}

// FanController is the board's fan controller.
type FanController struct {
	Part      string `yaml:"part"`
	SMBusAddr `yaml:",inline"`
	// Count is how many bays it drives.
	Count int `yaml:"count"`
	// MaxPWM is the value that means full speed.
	//
	// ⚠ NOT ALWAYS 255, AND GETTING IT WRONG IS NOT COSMETIC. Arista's own
	// driver defines the register as 0-255, but on the 7050TX-64 EOS reports
	// "Configured Speed 71%" while the register holds 127, and 71% of 180 is
	// 127.8. Scaling by 255 there commands about 140% of full scale for every
	// duty above 70, so the whole top of the range clamps to the same speed
	// and the curve stops meaning anything.
	MaxPWM int `yaml:"max_pwm"`
	// FloorPercent is the lowest duty this board will ever command.
	//
	// A fan controller that can be told to stop is one that will eventually be
	// told to stop by a bug, so this is enforced at the write and not by the
	// caller. Where a board states the vendor's own lowest setting, use that:
	// "never quieter than the vendor's own policy" is a floor with a reason
	// behind it rather than a number someone liked.
	FloorPercent int `yaml:"floor_percent"`
}

// SMBusParts are the temperature parts a board may name, and the register a
// whole-degree Celsius reading comes from on each.
//
// A remote diode is a separate entry rather than a flag because that is how a
// board sees it: one part, two sensors, in two different places in the
// chassis, and the board names both.
var SMBusParts = map[string]int{
	"max6658":        0x00, // local diode
	"max6658-remote": 0x01, // remote diode 1
	"lm73":           0x00, // 16-bit, and the high byte is whole degrees
}

// Validate reports what a board got wrong, by name, rather than letting it
// reach the hardware.
func (m *SMBusMap) Validate() error {
	if m == nil {
		return nil
	}
	seen := map[string]bool{}
	for i, s := range m.Sensors {
		switch {
		case s.Name == "":
			return fmt.Errorf("smbus sensor %d has no name", i)
		case seen[s.Name]:
			return fmt.Errorf("smbus sensor %q is listed twice", s.Name)
		}
		seen[s.Name] = true
		if _, ok := SMBusParts[s.Part]; !ok {
			return fmt.Errorf("smbus sensor %q: unknown part %q", s.Name, s.Part)
		}
		if err := s.SMBusAddr.validate(s.Name); err != nil {
			return err
		}
	}
	f := m.Fans
	if f == nil {
		return nil
	}
	if err := f.SMBusAddr.validate("fan controller"); err != nil {
		return err
	}
	switch {
	case f.Count < 1 || f.Count > 16:
		return fmt.Errorf("fan controller: %d bays is outside 1..16", f.Count)
	case f.MaxPWM < 1 || f.MaxPWM > 255:
		return fmt.Errorf("fan controller: max_pwm %d is outside 1..255", f.MaxPWM)
	case f.FloorPercent < 1 || f.FloorPercent > 100:
		// Zero is rejected rather than defaulted: a board that forgot to state
		// a floor must not get one that lets the fans stop.
		return fmt.Errorf("fan controller: floor_percent %d is outside 1..100", f.FloorPercent)
	}
	return nil
}

func (a SMBusAddr) validate(what string) error {
	switch {
	case a.Accel < 0 || a.Accel > 15:
		return fmt.Errorf("%s: accelerator %d is outside 0..15", what, a.Accel)
	case a.Bus < 0 || a.Bus > 15:
		return fmt.Errorf("%s: bus %d is outside 0..15", what, a.Bus)
	case a.Addr < 0x08 || a.Addr > 0x77:
		// Outside the addressable range an SMBus transfer is not a wrong
		// reading, it is a malformed one.
		return fmt.Errorf("%s: address %#02x is not a 7-bit SMBus address", what, a.Addr)
	}
	return nil
}
