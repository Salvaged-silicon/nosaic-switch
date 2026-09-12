package scd

import "github.com/salvaged-silicon/nosaic-switch/internal/platformhal"

func init() {
	platformhal.Register("scd", func(cfg platformhal.Config) (platformhal.HAL, error) {
		return Open(cfg)
	})
}
