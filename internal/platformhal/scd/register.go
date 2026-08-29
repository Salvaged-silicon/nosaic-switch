package scd

import "github.com/salvaged-silicon/nosaic-switch/internal/platformhal"

func init() {
	platformhal.Register("scd", func(pci, asicPCI string) (platformhal.HAL, error) {
		return Open(pci, asicPCI)
	})
}
