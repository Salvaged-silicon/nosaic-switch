package platformhal

import "fmt"

// Opener constructs a board's HAL from its addresses.
type Opener func(pci, asicPCI string) (HAL, error)

var drivers = map[string]Opener{}

// Register makes a driver available by name. Drivers register from their own
// package's init, so a board asking for "scd" gets one only if the driver is
// linked in -- which keeps the board data honest about what exists.
func Register(name string, o Opener) { drivers[name] = o }

// Open returns the HAL for a driver name.
func Open(driver, pci, asicPCI string) (HAL, error) {
	if driver == "" {
		return nil, fmt.Errorf("%w: this board declares no platform HAL driver", ErrUnsupported)
	}
	o, ok := drivers[driver]
	if !ok {
		return nil, fmt.Errorf("unknown platform HAL driver %q", driver)
	}
	return o(pci, asicPCI)
}
