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
	// Accelerators is where each SMBus accelerator block lives, by index.
	// Optional: a board that leaves it out gets the regular stride, which is
	// right on some boards and not on others -- see scdsmbus.NewAt.
	Accelerators []int `yaml:"accelerators"`

	Sensors []SMBusSensor  `yaml:"sensors"`
	Fans    *FanController `yaml:"fans"`
	// Retimer is a signal repeater between the ASIC and some cages, if the
	// board has one. Its tuning is not here -- see scd.LoadRetimerTuning.
	Retimer *SMBusAddr `yaml:"retimer"`
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

	// LM90-compatible: local diode at 0x00, remote at 0x01, both signed whole
	// degrees. Named for the register layout rather than a part number on
	// purpose -- the 7150S-52's sensor answers manufacturer ID (0xfe) 0x01 and
	// device ID (0xff) 0x11, which is not a Maxim part and so not a max6658
	// however identical the two registers are. Calling it what it reads is
	// honest; calling it a max6658 would be a guess that happens to work.
	"lm90":        0x00,
	"lm90-remote": 0x01,
}

// Validate reports what a board got wrong, by name, rather than letting it
// reach the hardware.
func (m *SMBusMap) Validate() error {
	if m == nil {
		return nil
	}
	for i, b := range m.Accelerators {
		if b < 0 || b > 0xfffff {
			return fmt.Errorf("smbus accelerator %d: base %#x is not a register offset", i, b)
		}
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
	if m.Retimer != nil {
		if err := m.Retimer.validate("retimer"); err != nil {
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

// CageTable is a board's front-panel transceiver cages.
//
// ⚠ ALSO BOARD DATA, AND FOR THE SAME REASON AS THE SMBus MAP ABOVE.
//
// The SCD gates every cage's laser, and where that table lives differs per
// board: the 7050SX2 has 54 cages at 0xa010 and the 7050TX-64 has 4 at 0xa100,
// because the TX's first 48 ports are RJ45 and have no cage at all. Driving the
// TX with the SX2's numbers writes 54 entries starting at 0xa010 -- straight
// through that board's per-cage LED block and out the far side of the real
// control table.
//
// It is also what the cages being dark looks like: the module is held in reset
// and low power, so it answers nothing and emits nothing, while the switch chip
// reports a port that is enabled, at the right speed, with the right lane map.
type CageTable struct {
	// Base and Stride locate the per-cage control words.
	Base   int `yaml:"base"`
	Stride int `yaml:"stride"`
	// Count is how many cages the front panel has, and SFPCount how many of
	// those come first as SFP+; the rest are QSFP+. A board of nothing but
	// QSFP+ states sfp_count: 0.
	Count    int `yaml:"count"`
	SFPCount int `yaml:"sfp_count"`
	// SFPEEPROM and QSFPEEPROM say which SMBus accelerator and bus reach each
	// range's module EEPROM: accel = accel_base + index/buses_per_accel, and
	// bus = index % buses_per_accel, with index counted from the start of that
	// range. SFPEEPROM may be omitted on a board with no SFP+ cages.
	SFPEEPROM  *CageEEPROM `yaml:"sfp_eeprom"`
	QSFPEEPROM *CageEEPROM `yaml:"qsfp_eeprom"`
}

// CageEEPROM is how one range of cages is laid out across the SMBus.
type CageEEPROM struct {
	AccelBase     int `yaml:"accel_base"`
	BusesPerAccel int `yaml:"buses_per_accel"`
}

// Validate reports what a board got wrong about its cages.
func (c *CageTable) Validate() error {
	if c == nil {
		return nil
	}
	switch {
	case c.Base <= 0:
		return fmt.Errorf("cages: base %#x is not a register offset", c.Base)
	case c.Stride <= 0:
		return fmt.Errorf("cages: stride %d must be positive", c.Stride)
	case c.Count < 1 || c.Count > 256:
		return fmt.Errorf("cages: count %d is outside 1..256", c.Count)
	case c.SFPCount < 0 || c.SFPCount > c.Count:
		return fmt.Errorf("cages: sfp_count %d is outside 0..%d", c.SFPCount, c.Count)
	case c.SFPCount > 0 && c.SFPEEPROM == nil:
		return fmt.Errorf("cages: %d SFP+ cages but no sfp_eeprom", c.SFPCount)
	case c.SFPCount < c.Count && c.QSFPEEPROM == nil:
		return fmt.Errorf("cages: %d QSFP+ cages but no qsfp_eeprom", c.Count-c.SFPCount)
	}
	for name, e := range map[string]*CageEEPROM{"sfp_eeprom": c.SFPEEPROM, "qsfp_eeprom": c.QSFPEEPROM} {
		if e == nil {
			continue
		}
		if e.AccelBase < 0 || e.AccelBase > 15 {
			return fmt.Errorf("cages: %s accel_base %d is outside 0..15", name, e.AccelBase)
		}
		if e.BusesPerAccel < 1 || e.BusesPerAccel > 16 {
			return fmt.Errorf("cages: %s buses_per_accel %d is outside 1..16", name, e.BusesPerAccel)
		}
	}
	return nil
}

// CageSMBus is which accelerator and bus reach one cage's module EEPROM.
// Cages are 1-based, matching the front panel.
func (c *CageTable) CageSMBus(cage int) (accel, bus int) {
	e, idx := c.QSFPEEPROM, cage-1-c.SFPCount
	if cage <= c.SFPCount {
		e, idx = c.SFPEEPROM, cage-1
	}
	if e == nil {
		return -1, -1
	}
	return e.AccelBase + idx/e.BusesPerAccel, idx % e.BusesPerAccel
}

// ResetLine is one reset the board wants released during bring-up, beyond the
// switch chip's own.
//
// ⚠ A BOARD CAN HOLD MORE THAN THE ASIC IN RESET, AND THE REST IS INVISIBLE.
//
// The 7050TX-64 puts a TI DS100KR800 retimer in front of its last two QSFP
// cages and holds it in reset from power-on, on bit 8 of the same block as the
// switch chip. Releasing only the chip leaves those two cages fed by a part
// that passes nothing: the ASIC transmits, its MAC counters climb, and the
// module reports loss of signal on all four transmit lanes because nothing
// electrical ever arrives at it. The near end still locks onto the far end's
// light and reports the port up at 40000, so every status on this side says
// the link is healthy while the far end never links at all.
//
// Which bits exist and what they gate is per board -- the sibling 7050SX2 has
// no retimer and no bit 8 -- so they are named here rather than compiled in.
type ResetLine struct {
	// Name is what it gates, for the log. "qsfp-retimer", not "bit 8".
	Name string `yaml:"name"`
	Bit  uint   `yaml:"bit"`
}

// ValidateResets reports a reset line a board cannot have meant.
func ValidateResets(rs []ResetLine) error {
	seen := map[uint]bool{}
	for _, r := range rs {
		switch {
		case r.Name == "":
			return fmt.Errorf("reset on bit %d has no name", r.Bit)
		case r.Bit > 31:
			return fmt.Errorf("reset %q: bit %d is outside a 32-bit register", r.Name, r.Bit)
		case r.Bit <= 1:
			// Bits 0 and 1 are the switch chip's own and are released by the
			// bring-up sequence, which waits for the device to appear. Listing
			// one here would release it a second time, out of that order.
			return fmt.Errorf("reset %q: bit %d is the switch chip's own and is "+
				"released by the bring-up sequence", r.Name, r.Bit)
		case seen[r.Bit]:
			return fmt.Errorf("reset %q: bit %d is listed twice", r.Name, r.Bit)
		}
		seen[r.Bit] = true
	}
	return nil
}
