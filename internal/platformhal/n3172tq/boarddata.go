/* SPDX-License-Identifier: Apache-2.0 */
package n3172tq

import (
	"fmt"
	"regexp"
)

// Data is everything this board states about its platform hardware.
//
// One board-owned struct, reached through platformhal.Config.BoardData. It is
// deliberately not shared with another board's HAL and deliberately not a
// typed field in Config: the Nexus 3172TQ's sensors happen to be described the
// same way a future board's might be, and the moment that shape is shared the
// two boards have to agree on it forever.
type Data struct {
	// Hwmon names the parts the kernel has already bound drivers to.
	Hwmon *Map `yaml:"hwmon"`
	// I2C is what the image build instantiates at boot, because x86 has no
	// device tree to do it. The driver also needs it to resolve mux channels.
	I2C *KernelI2C `yaml:"i2c"`
}

// Validate reports everything wrong with this board's platform data.
func (d *Data) Validate() error {
	if d == nil {
		return nil
	}
	if err := d.Hwmon.Validate(); err != nil {
		return err
	}
	return d.I2C.Validate()
}

// Map is how a board whose parts the KERNEL drives states what they are.
//
// The difference from SMBusMap is which side owns the bus. On the Arista boards
// the SCD's SMBus is driven from userspace, so the board states register bases
// and the driver does the transfers. Here the parts are ordinary i2c devices
// with in-tree drivers -- an ADT7462, a couple of PMBus supplies -- and the
// kernel has already read them by the time anything asks. What a board has to
// state is not how to reach them but which one is which:
//
//   - "temp1" is not a location. NX-OS calls them Front-Left, Front-Right and
//     Back, and an operator reading a thermal log needs the location, not the
//     channel number.
//   - both power supplies are the same part at the same address on different
//     mux channels, so nothing in sysfs distinguishes them except where they
//     hang.
type Map struct {
	Sensors []Sensor `yaml:"sensors"`
	Fans    *Fans    `yaml:"fans"`
	PSUs    []PSU    `yaml:"psus"`
}

// Sensor is one temperature channel of one chip, and what it measures.
type Sensor struct {
	// Name is the location, as an operator would say it.
	Name string `yaml:"name"`
	// Chip is the hwmon driver's name, as /sys/class/hwmon/hwmonN/name reads.
	Chip string `yaml:"chip"`
	// Channel is N in tempN_input, 1-based as hwmon numbers them.
	Channel int `yaml:"channel"`
}

// Fans is the controller a board's fans hang off.
type Fans struct {
	// Chip is the hwmon name of the controller.
	Chip string `yaml:"chip"`
	// Count is how many bays the chassis has. Stated rather than counted from
	// sysfs: a controller with eight tachometer inputs and four fitted fans
	// reports eight, and four of them read zero forever.
	Count int `yaml:"count"`
	// FloorPercent is the lowest duty this board will command. A controller
	// that can be told to stop is one that a bug will eventually tell to stop.
	FloorPercent int `yaml:"floor_percent"`
}

// PSU is one supply, identified by where it hangs rather than by name:
// two identical parts at one address are told apart only by mux channel.
type PSU struct {
	Name string `yaml:"name"`
	Chip string `yaml:"chip"`
	// Address on its bus, and the mux channel it is behind.
	Address int  `yaml:"address"`
	Channel *int `yaml:"channel"`
}

// Validate rejects a map that would read the wrong thing rather than fail.
//
// A sensor naming a channel its chip does not have reads nothing, and a
// thermal loop with one fewer sensor than it thinks does not complain -- it
// just tracks a cooler part of the board than the one that is overheating.
func (m *Map) Validate() error {
	if m == nil {
		return nil
	}
	seen := map[string]bool{}
	for i, s := range m.Sensors {
		switch {
		case s.Name == "":
			return fmt.Errorf("hwmon sensor %d has no name", i)
		case seen[s.Name]:
			return fmt.Errorf("hwmon sensor %q is listed twice", s.Name)
		case s.Chip == "":
			return fmt.Errorf("hwmon sensor %q names no chip", s.Name)
		case s.Channel < 1:
			return fmt.Errorf("hwmon sensor %q: channel %d; hwmon numbers "+
				"tempN_input from 1", s.Name, s.Channel)
		}
		seen[s.Name] = true
	}
	if f := m.Fans; f != nil {
		switch {
		case f.Chip == "":
			return fmt.Errorf("hwmon fans name no chip")
		case f.Count < 1:
			return fmt.Errorf("hwmon fans: count %d", f.Count)
		case f.FloorPercent < 1 || f.FloorPercent > 100:
			return fmt.Errorf("hwmon fans: floor_percent %d is not a duty; a "+
				"floor of 0 is a board that can be told to stop cooling",
				f.FloorPercent)
		}
	}
	names := map[string]bool{}
	for i, p := range m.PSUs {
		switch {
		case p.Name == "":
			return fmt.Errorf("hwmon psu %d has no name", i)
		case names[p.Name]:
			return fmt.Errorf("hwmon psu %q is listed twice", p.Name)
		case p.Chip == "":
			return fmt.Errorf("hwmon psu %q names no chip", p.Name)
		case p.Address < 0x03 || p.Address > 0x77:
			return fmt.Errorf("hwmon psu %q: address %#x is outside 0x03-0x77",
				p.Name, p.Address)
		}
		names[p.Name] = true
	}
	return nil
}

// KernelI2C says which i2c parts a board has and where they answer, so the
// kernel can be told about them at boot.
//
// # WHY A BOARD HAS TO SAY THIS AT ALL
//
// x86 has no device tree. On an embedded board the parts behind an i2c mux are
// described in the DTS and the kernel instantiates them itself; on x86 nothing
// declares them, so a board whose sensor has a perfectly good in-tree driver
// still gets no hwmon entry -- the module is loaded and has nothing to bind to.
// That is exactly what the Nexus 3172TQ did: i801 up, mux present, every
// sensor driver built, and /sys/class/hwmon holding nothing but acpitz.
//
// The alternative is a platform HAL driver per board, which is the right answer
// for a board whose controller has to be driven (Arista's SCD) and far too much
// for a board whose parts are all standard and merely undeclared.
type KernelI2C struct {
	// Adapter is matched against the parent adapter's name, not its number.
	//
	// ⚠ NUMBERS COME FROM PROBE ORDER, and a mux adds more of them the moment
	// its driver binds -- each channel becomes its own adapter. A hardcoded 0
	// stops meaning what it meant somewhere between one boot and the next.
	Adapter string `yaml:"adapter"`

	// Mux is the multiplexer the rest sit behind, if there is one. It is
	// instantiated first, and the channels below are its channels.
	Mux *I2CDevice `yaml:"mux"`

	// Devices are instantiated in the order given.
	Devices []I2CDevice `yaml:"devices"`
}

// I2CDevice is one part: which driver should claim it, where it answers, and
// which mux channel it is behind.
type I2CDevice struct {
	// Driver is the name the kernel matches on -- the i2c_device_id, which is
	// not always the module name: the at24 module answers to "24c512", the
	// pmbus module to "pmbus".
	Driver string `yaml:"driver"`

	// Address on its own bus.
	Address int `yaml:"address"`

	// Channel is the mux channel this part is behind. Absent means it is on
	// the parent adapter. A pointer because channel 0 is a real answer and
	// "not behind the mux" has to be distinguishable from it.
	Channel *int `yaml:"channel"`

	// Note is what the part is for. Carried into the generated script so that
	// a boot which fails to instantiate something says what was lost.
	Note string `yaml:"note"`
}

// i2c driver names are kernel identifiers, not free text.
var i2cDriverRE = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]*$`)

// Validate checks what a wrong value would otherwise do quietly.
//
// An out-of-range address does not fail loudly: new_device accepts it, the
// driver binds to nothing, and the board comes up with no sensors and no error.
func (k *KernelI2C) Validate() error {
	if k == nil {
		return nil
	}
	if k.Adapter == "" && (k.Mux != nil || len(k.Devices) > 0) {
		return fmt.Errorf("adapter is required: the parent bus is matched by " +
			"name, because adapter numbers come from probe order")
	}
	if k.Mux != nil {
		if k.Mux.Channel != nil {
			return fmt.Errorf("mux: has a channel, but the mux is what makes " +
				"channels; it sits on the parent adapter")
		}
		if err := k.Mux.validate("mux"); err != nil {
			return err
		}
	}
	seen := map[string]int{}
	for i, d := range k.Devices {
		what := fmt.Sprintf("devices[%d]", i)
		if err := d.validate(what); err != nil {
			return err
		}
		if d.Channel != nil && k.Mux == nil {
			return fmt.Errorf("%s: behind channel %d, but no mux is declared",
				what, *d.Channel)
		}
		// Two parts at one address on one bus is a board description that
		// cannot be true, and the second new_device fails at boot.
		//
		// ⚠ The channel is compared by VALUE. Channel is a *int, so keying on
		// the pointer compares addresses and never matches -- which is how the
		// first version of this accepted two parts at 0x58 on channel 0.
		bus := "parent"
		if d.Channel != nil {
			bus = fmt.Sprintf("ch%d", *d.Channel)
		}
		key := fmt.Sprintf("%s/%#x", bus, d.Address)
		if prev, dup := seen[key]; dup {
			return fmt.Errorf("%s: address %#x on the same bus as devices[%d]",
				what, d.Address, prev)
		}
		seen[key] = i
	}
	return nil
}

func (d *I2CDevice) validate(what string) error {
	if !i2cDriverRE.MatchString(d.Driver) {
		return fmt.Errorf("%s: driver %q is not a kernel i2c device name", what, d.Driver)
	}
	// 0x00-0x02 and 0x78-0x7f are reserved by the i2c specification.
	if d.Address < 0x03 || d.Address > 0x77 {
		return fmt.Errorf("%s: address %#x is outside the addressable range 0x03-0x77",
			what, d.Address)
	}
	if d.Channel != nil && (*d.Channel < 0 || *d.Channel > 63) {
		return fmt.Errorf("%s: channel %d is not a mux channel", what, *d.Channel)
	}
	return nil
}
