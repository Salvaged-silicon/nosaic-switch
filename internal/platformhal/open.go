package platformhal

import "fmt"

// Config is everything a board states about its platform hardware.
//
// A struct rather than an argument list because the addresses a driver needs
// are board data that grows: the SMBus placement below was a hardcoded
// constant until a second Arista board turned out to put its fan controller on
// a different bus.
type Config struct {
	// PCI is where the controller is, in full domain:bus:dev.fn form.
	PCI string
	// ASICPCI is where the switch chip appears once released from reset.
	ASICPCI string
	// SMBus is where the board's sensors and fan controller sit. Optional:
	// a driver that needs it says so itself, so a board with no SMBus
	// devices is not obliged to invent an empty section.
	SMBus *SMBusMap
	// Cages is the board's front-panel transceiver table. Optional in the
	// same way and for the same reason: a board with no cages states none.
	Cages *CageTable
	// Resets are board reset lines released during bring-up, beyond the
	// switch chip's own.
	Resets []ResetLine
	// I2C is where the board's platform devices sit on its Linux i2c buses,
	// for boards whose controller is not an SCD. Optional and mutually
	// exclusive with SMBus in practice, though nothing enforces that: a
	// driver asks for the one it needs and says so when it is absent.
	I2C *I2CMap
}

// Opener constructs a board's HAL from its configuration.
type Opener func(Config) (HAL, error)

var drivers = map[string]Opener{}

// Register makes a driver available by name. Drivers register from their own
// package's init, so a board asking for "scd" gets one only if the driver is
// linked in -- which keeps the board data honest about what exists.
func Register(name string, o Opener) { drivers[name] = o }

// Open returns the HAL for a driver name.
func Open(driver string, cfg Config) (HAL, error) {
	if driver == "" {
		return nil, fmt.Errorf("%w: this board declares no platform HAL driver", ErrUnsupported)
	}
	o, ok := drivers[driver]
	if !ok {
		return nil, fmt.Errorf("unknown platform HAL driver %q", driver)
	}
	if err := cfg.SMBus.Validate(); err != nil {
		return nil, fmt.Errorf("this board's smbus map: %w", err)
	}
	if err := cfg.Cages.Validate(); err != nil {
		return nil, fmt.Errorf("this board's cage table: %w", err)
	}
	if err := ValidateResets(cfg.Resets); err != nil {
		return nil, fmt.Errorf("this board's reset lines: %w", err)
	}
	if err := cfg.I2C.Validate(); err != nil {
		return nil, fmt.Errorf("this board's i2c map: %w", err)
	}
	return o(cfg)
}
