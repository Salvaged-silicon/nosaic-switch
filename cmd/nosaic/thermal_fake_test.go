package main

import (
	"context"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// halShim carries a fan controller through the HAL interface the command
// takes. Everything the cooling loop does not use is unsupported, which is
// also a check that it does not quietly depend on more than it declares.
type halShim struct{ fc fanController }

func halOf(fc fanController) platformhal.HAL { return &halShim{fc} }

func (h *halShim) Temperatures() (map[string]int, error) { return h.fc.Temperatures() }
func (h *halShim) SetAllFansPercent(p int) (int, error)  { return h.fc.SetAllFansPercent(p) }
func (h *halShim) Board() (platformhal.Identity, error) {
	return platformhal.Identity{}, platformhal.ErrUnsupported
}
func (h *halShim) ResetState(platformhal.Reset) (bool, error) {
	return false, platformhal.ErrUnsupported
}
func (h *halShim) ReleaseSwitchChip(context.Context) error { return platformhal.ErrUnsupported }
func (h *halShim) Watchdog() (platformhal.Watchdog, error) { return nil, platformhal.ErrUnsupported }
func (h *halShim) PSUPresent() (map[string]bool, error)    { return nil, platformhal.ErrUnsupported }
