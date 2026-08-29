package scd

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// Board identifies the switch.
//
// On Arista hardware this comes from a prefdl structure on an i2c SEEPROM, and
// NOSaic does not read it yet -- the SEEPROM's bus and address have not been
// established on this board, and guessing at addresses on an SMBus carrying
// PSU controllers is not a safe way to find out. Until it is read, saying so
// is the correct answer: an identity invented from the build configuration
// would be a switch reporting what it was compiled for rather than what it is.
func (s *SCD) Board() (platformhal.Identity, error) {
	return platformhal.Identity{}, fmt.Errorf(
		"%w: board identity needs the prefdl SEEPROM, which is not located yet "+
			"(see platform/arista-7050sx2-72q/docs/hardware.md)", platformhal.ErrUnsupported)
}

// Temperatures reports every thermal sensor the kernel exposes, in
// millidegrees Celsius, keyed by the sensor's own label.
//
// This reads hwmon rather than driving the SMBus directly. The sensors on this
// board are ordinary lm-sensors parts behind the SCD's SMBus, so once the i2c
// adapter is bound the kernel's own drivers present them -- and going through
// hwmon means the readings come from the same place `sensors` would show, with
// no second implementation to disagree with it.
//
// An empty map with no error means the drivers are not bound yet, which is a
// different situation from a board with no sensors and is reported as such.
func (s *SCD) Temperatures() (map[string]int, error) {
	dirs, err := filepath.Glob("/sys/class/hwmon/hwmon*")
	if err != nil {
		return nil, err
	}
	out := map[string]int{}
	for _, dir := range dirs {
		chip := readTrim(filepath.Join(dir, "name"))
		inputs, _ := filepath.Glob(filepath.Join(dir, "temp*_input"))
		for _, in := range inputs {
			milli, err := readInt(in)
			if err != nil {
				continue
			}
			// A label is the sensor's own name for itself ("Board", "Cpu");
			// where there is none the chip and index still identify it
			// uniquely, which matters when two chips both report temp1.
			name := readTrim(strings.TrimSuffix(in, "_input") + "_label")
			if name == "" {
				name = chip + ":" + strings.TrimSuffix(filepath.Base(in), "_input")
			} else if chip != "" {
				name = chip + ":" + name
			}
			out[name] = milli
		}
	}
	if len(out) == 0 {
		return out, fmt.Errorf("no hwmon sensors are present; the i2c adapter behind " +
			"the SCD may not be bound (CONFIG_I2C_PIIX4)")
	}
	return out, nil
}

// PSUPresent reports which supplies are fitted.
//
// Presence is a GPIO, not a reading from the supply itself -- which is the
// point: it answers for a slot that is empty or holds a dead supply, where
// PMBus can only answer for one that is alive enough to talk.
//
// The bit positions differ per platform and have not been read off this board,
// so this reports ErrUnsupported rather than a guess. A wrong bit here reads a
// present supply as missing, and the response to that is someone walking to
// the rack.
func (s *SCD) PSUPresent() (map[string]bool, error) {
	return nil, fmt.Errorf("%w: PSU presence GPIO bits are not established for this board",
		platformhal.ErrUnsupported)
}

func readTrim(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

func readInt(path string) (int, error) {
	s := readTrim(path)
	if s == "" {
		return 0, fmt.Errorf("empty")
	}
	return strconv.Atoi(s)
}

// SCD implements the full board contract.
var _ platformhal.HAL = (*SCD)(nil)
