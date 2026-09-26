package scd

import (
	"fmt"
	"os"
	"strconv"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal/prefdl"
)

// Board identifies the switch, from the prefdl structure on its identity
// EEPROM.
//
// ⚠ THE EEPROM IS ON A HOST i2c BUS, NOT ON THE SCD. Everything else this
// driver reads goes through the controller's SMBus accelerators, and the
// identity EEPROM does not -- on a 7150S-52 it is address 0x52 on the
// southbridge bus that also carries the board's CPLD. That is why the location
// is board data rather than something this file knows.
//
// A board that has not stated one says so. An identity invented from the build
// configuration would be a switch reporting what it was compiled for rather
// than what it is, which is worse than no answer -- the entire point of
// reading it is that a running switch can be identified from itself.
func (s *SCD) Board() (platformhal.Identity, error) {
	if s.prefdl == nil {
		return platformhal.Identity{}, fmt.Errorf(
			"%w: this board does not state where its identity EEPROM is "+
				"(platform_hal.prefdl in board.yml)", platformhal.ErrUnsupported)
	}
	p, err := prefdl.Read(s.prefdl.Bus, s.prefdl.Addr)
	if err != nil {
		return platformhal.Identity{}, fmt.Errorf(
			"%w: reading the prefdl at i2c-%d %#02x: %v",
			platformhal.ErrUnsupported, s.prefdl.Bus, s.prefdl.Addr, err)
	}
	return platformhal.Identity{
		Model:    p.SKU,
		Serial:   p.Serial,
		Revision: p.HwRev,
		SID:      p.SID,
	}, nil
}

// BoardMAC is the base MAC address the board was manufactured with, in
// colon-separated form, or "" with an error if it cannot be read.
//
// Separate from Board() because it has a different caller and a different
// consequence: an image that cannot name its model is untidy, and an image
// that cannot name its MAC comes up with a random one.
func (s *SCD) BoardMAC() (string, error) {
	if s.prefdl == nil {
		return "", fmt.Errorf("%w: this board does not state where its "+
			"identity EEPROM is", platformhal.ErrUnsupported)
	}
	p, err := prefdl.Read(s.prefdl.Bus, s.prefdl.Addr)
	if err != nil {
		return "", err
	}
	mac := p.MACAddress()
	if mac == "" {
		return "", fmt.Errorf("the prefdl at i2c-%d %#02x decoded but carries "+
			"no MAC", s.prefdl.Bus, s.prefdl.Addr)
	}
	return mac, nil
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
