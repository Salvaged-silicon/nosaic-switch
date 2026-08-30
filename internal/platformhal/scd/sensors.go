package scd

import (
	"fmt"
	"os"
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
