package platformhal

import "fmt"

// Where a board's platform devices sit on its Linux i2c buses.
//
// The counterpart to SMBusMap, and separate from it because the two describe
// genuinely different machines. An Arista board reaches its sensors through
// the SCD's own SMBus accelerators, which are windows in a PCI BAR and are
// numbered by the SCD. A board like the AS4610 has ordinary i2c controllers in
// its SoC, and Linux gives each an adapter number — so the address of a device
// is a bus number and a 7-bit address, and there is no accelerator in it.
//
// ⚠ BUS NUMBERS ARE STATED, NOT DISCOVERED, AND THEN CHECKED.
//
// Linux adapter numbers come from probe order, so in principle they are not
// stable and the temptation is to hunt for the right one at runtime. This
// project's rule is the other way round — a bus hunted for at runtime is a bus
// that can be found wrong — so the board states them and the driver verifies
// that what it finds there is what the board said would be there. Stating them
// makes a renumbering an error with a name in it; hunting for them makes it a
// switch that reads a fan tachometer off a transceiver.
type I2CMap struct {
	// Controller is the board's own controller — a CPLD on every board that
	// has one of these — carrying fans, power supply status and reset lines.
	Controller I2CAddr `yaml:"controller"`

	// Adapter is the name Linux gives the controller's adapter, as it appears
	// in /sys/class/i2c-adapter/i2c-N/name. Optional, and the whole point of
	// stating it: it is what turns "bus 0 is not what we thought" from a wrong
	// reading into a refusal.
	Adapter string `yaml:"adapter"`

	Sensors []I2CSensor `yaml:"sensors"`

	// Cages is the front-panel transceivers, which sit behind a mux: one bus
	// per cage, consecutive, in front-panel order.
	Cages *I2CCages `yaml:"cages"`

	// EEPROM is the board identity EEPROM, if the board has one.
	EEPROM *I2CAddr `yaml:"eeprom"`
}

// I2CAddr locates one device: a Linux bus number and a 7-bit address.
type I2CAddr struct {
	Bus  int `yaml:"bus"`
	Addr int `yaml:"addr"`
}

// I2CSensor is one temperature sensor.
//
// Name describes what the part measures rather than what the part is, because
// that is what somebody reading `nosaic platform status` wants to know.
type I2CSensor struct {
	Name    string `yaml:"name"`
	Part    string `yaml:"part"`
	I2CAddr `yaml:",inline"`
}

// I2CCages is the front-panel transceiver bus range.
//
// Consecutive by construction: they are the channels of one mux, and Linux
// numbers a mux's channels consecutively from the first free adapter number.
// A board whose cages are not consecutive would need a list instead, and none
// of them are.
type I2CCages struct {
	FirstBus int `yaml:"first_bus"`
	Count    int `yaml:"count"`
	// Addr is the transceiver's low i2c address, 0x50 on every SFF module
	// ever made. Stated anyway so that a board with something unusual in
	// front of its cages has somewhere to say so.
	Addr int `yaml:"addr"`
}

// I2CParts are the temperature parts a board may name, and the register a
// reading comes from on each.
//
// The LM77 is 16-bit and left-justified: the reading is in the top 13 bits of
// a big-endian word, in units of 0.5 °C, with the bottom three bits status.
// That is not something a caller should have to know, so the decode lives in
// the driver and this map only says which register to read.
var I2CParts = map[string]int{
	"lm77": 0x00,
}

// Validate reports what a board got wrong, by name, rather than letting it
// reach the bus.
//
// A wrong i2c address does not fail loudly. It reads a device that is not
// there, and the controller reports every command refused — which reads as a
// broken controller rather than as a typo, and has cost this project a day
// once already on a different board.
func (m *I2CMap) Validate() error {
	if m == nil {
		return nil
	}
	if err := m.Controller.validate("controller"); err != nil {
		return err
	}
	for i, s := range m.Sensors {
		what := fmt.Sprintf("sensors[%d]", i)
		if s.Name == "" {
			return fmt.Errorf("%s: name is required — it is what the reading is called", what)
		}
		if _, ok := I2CParts[s.Part]; !ok {
			return fmt.Errorf("%s (%s): part %q is not one this knows", what, s.Name, s.Part)
		}
		if err := s.I2CAddr.validate(what); err != nil {
			return err
		}
	}
	if c := m.Cages; c != nil {
		if c.Count <= 0 {
			return fmt.Errorf("cages: count must be positive, not %d", c.Count)
		}
		if c.FirstBus < 0 {
			return fmt.Errorf("cages: first_bus %d is not a bus number", c.FirstBus)
		}
		if c.Addr != 0 && (c.Addr < 0x08 || c.Addr > 0x77) {
			return fmt.Errorf("cages: addr %#x is outside the 7-bit range 0x08..0x77", c.Addr)
		}
	}
	if e := m.EEPROM; e != nil {
		if err := e.validate("eeprom"); err != nil {
			return err
		}
	}
	return nil
}

func (a I2CAddr) validate(what string) error {
	if a.Bus < 0 {
		return fmt.Errorf("%s: bus %d is not a bus number", what, a.Bus)
	}
	// 0x00..0x07 and 0x78..0x7f are reserved by the i2c specification, so an
	// address in either is a transcription error rather than a device.
	if a.Addr < 0x08 || a.Addr > 0x77 {
		return fmt.Errorf("%s: address %#x is outside the 7-bit range 0x08..0x77",
			what, a.Addr)
	}
	return nil
}

// CageAddr returns the i2c address transceivers answer on, defaulting to the
// one every SFF module uses.
func (c *I2CCages) CageAddr() int {
	if c == nil || c.Addr == 0 {
		return 0x50
	}
	return c.Addr
}
