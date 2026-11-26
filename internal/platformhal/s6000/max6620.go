/* SPDX-License-Identifier: Apache-2.0 */

package s6000

import (
	"fmt"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// MAX6620 4-channel linear fan-speed controller register map (Maxim / Analog
// Devices MAX6620 datasheet, verified against live S6000-ON register dumps).
//
// Upstream Linux has no max6620 hwmon driver, so the S6000-ON HAL drives the
// two MAX6620 chips (0x29 and 0x2a on mux channel 1) directly over SMBus.
// On S6000-ON the chips operate in closed-loop RPM mode (Fan Config bit 7 = 1),
// where writing the 11-bit Target Tach Count (0x20..0x27) causes the chip to
// regulate the fan's DAC voltage (read back from 0x18..0x1f).
const (
	max6620RegGlobalCfg     = 0x00 // bit 7: RUN (0=run, 1=standby); bit 3: FULL
	max6620RegFanCfg0       = 0x02 // + ch (0..3): bit 7 = RPM_MODE (1=RPM), bits 6..5 = SR
	max6620RegTachMSB0      = 0x10 // + 2*ch: bits 7..0 = count[10:3] (RO)
	max6620RegTachLSB0      = 0x11 // + 2*ch: bits 7..5 = count[2:0] (RO)
	max6620RegDACMSB0       = 0x18 // + 2*ch: bits 7..0 = dac[8:1] (Current DAC, RO)
	max6620RegDACLSB0       = 0x19 // + 2*ch: bit 7     = dac[0]   (Current DAC, RO)
	max6620RegTargetTachMSB = 0x20 // + 2*ch: bits 7..0 = target_count[10:3] (R/W)
	max6620RegTargetTachLSB = 0x21 // + 2*ch: bits 7..5 = target_count[2:0]  (R/W, latches MSB)

	max6620GlobalRunMask = 0x80
	max6620FanRPMModeBit = 0x80
	max6620TachMax       = 0x7ff // 11-bit saturated count (stalled/absent fan)
	max6620DACMax        = 511   // 9-bit DAC full scale (100%)
	max6620OscHz         = 8192  // internal oscillator frequency
	max6620MaxRPM        = 20000 // full-speed fan RPM on Dell S6000-ON
)

// S6000-ON wires 3 fan trays x 2 fans = 6 fans across the two MAX6620
// controllers on mux channel 1: channels 0..3 on 0x29 and channels 0..1 on 0x2a.
// Confirmed by live register dump on S6000-ON hardware.
var max6620Chips = []struct {
	addr     int
	channels int
}{
	{0x29, 4},
	{0x2a, 2},
}

// max6620DecodeRPM converts an 11-bit tach count and fan config register into
// RPM (assuming 2 tach pulses per revolution and the internal 8192 Hz clock):
//
//	SR  = 1 << ((cfg >> 5) & 0x3)          // 1, 2, 4, or 8 tach periods
//	RPM = (60 * SR * 8192) / (2 * count)   // = (245760 * SR) / count
func max6620DecodeRPM(msb, lsb, fanCfg byte) int {
	count := (uint16(msb) << 3) | (uint16(lsb) >> 5)
	if count == 0 || count >= max6620TachMax {
		return 0
	}
	sr := 1 << ((fanCfg >> 5) & 0x03)
	return (60 * sr * max6620OscHz) / (2 * int(count))
}

// max6620DecodeDACPercent converts the 9-bit DAC MSB/LSB registers to 0..100%.
func max6620DecodeDACPercent(msb, lsb byte) int {
	dac := (int(msb) << 1) | (int(lsb) >> 7)
	return (dac*100 + max6620DACMax/2) / max6620DACMax
}

// max6620EncodeTargetTach converts a 0..100% speed request and fan config
// register into the 11-bit Target Tach Count MSB/LSB bytes for closed-loop
// RPM mode.
func max6620EncodeTargetTach(pct int, fanCfg byte) (msb, lsb byte) {
	if pct < floorPercent {
		pct = floorPercent
	}
	if pct > 100 {
		pct = 100
	}
	sr := 1 << ((fanCfg >> 5) & 0x03)
	targetRPM := (pct * max6620MaxRPM) / 100
	count := (60 * sr * max6620OscHz) / (2 * targetRPM)
	if count < 1 {
		count = 1
	}
	if count >= max6620TachMax {
		count = max6620TachMax - 1
	}
	return byte(count >> 3), byte((count & 0x7) << 5)
}

func readMax6620Fans(b smbus) ([]platformhal.Fan, error) {
	var fans []platformhal.Fan
	idx := 0
	var firstErr error
	for _, chip := range max6620Chips {
		addr := chip.addr
		for ch := 0; ch < chip.channels; ch++ {
			cfg, err := b.ReadReg(addr, max6620RegFanCfg0+ch)
			if err != nil {
				if firstErr == nil {
					firstErr = err
				}
				continue
			}
			tMSB, err := b.ReadReg(addr, max6620RegTachMSB0+2*ch)
			if err != nil {
				if firstErr == nil {
					firstErr = err
				}
				continue
			}
			tLSB, err := b.ReadReg(addr, max6620RegTachLSB0+2*ch)
			if err != nil {
				if firstErr == nil {
					firstErr = err
				}
				continue
			}
			dMSB, err := b.ReadReg(addr, max6620RegDACMSB0+2*ch)
			if err != nil {
				if firstErr == nil {
					firstErr = err
				}
				continue
			}
			dLSB, err := b.ReadReg(addr, max6620RegDACLSB0+2*ch)
			if err != nil {
				if firstErr == nil {
					firstErr = err
				}
				continue
			}

			idx++
			rpm := max6620DecodeRPM(tMSB, tLSB, cfg)
			pct := max6620DecodeDACPercent(dMSB, dLSB)
			dac := (int(dMSB) << 1) | (int(dLSB) >> 7)
			fans = append(fans, platformhal.Fan{
				Index:   idx,
				Present: rpm > 0,
				RPM:     rpm,
				Percent: pct,
				Raw:     fmt.Sprintf("%#02x:ch%d dac=%d", addr, ch, dac),
			})
		}
	}
	if len(fans) == 0 && firstErr != nil {
		return nil, firstErr
	}
	return fans, nil
}

func writeMax6620Percent(b smbus, pct int) (int, error) {
	if pct < floorPercent {
		pct = floorPercent
	}
	if pct > 100 {
		pct = 100
	}

	refused := 0
	wrote := 0
	for _, chip := range max6620Chips {
		addr := chip.addr
		// Ensure the chip is in normal run mode (bit 7 = 0) and not forced full.
		if gcfg, err := b.ReadReg(addr, max6620RegGlobalCfg); err == nil {
			if gcfg&(max6620GlobalRunMask|0x08) != 0 {
				_ = b.WriteReg(addr, max6620RegGlobalCfg, gcfg&^byte(max6620GlobalRunMask|0x08))
			}
		}
		for ch := 0; ch < chip.channels; ch++ {
			wrote++
			cfg := byte(0xa8) // RPM_MODE=1, SR=2, TACH_EN=1
			if cur, err := b.ReadReg(addr, max6620RegFanCfg0+ch); err == nil {
				cfg = cur | max6620FanRPMModeBit
				if cur&max6620FanRPMModeBit == 0 {
					_ = b.WriteReg(addr, max6620RegFanCfg0+ch, cfg)
				}
			}
			tMSB, tLSB := max6620EncodeTargetTach(pct, cfg)
			// Write MSB then LSB (MAX6620 latches the 11-bit target tach on LSB write).
			if err := b.WriteReg(addr, max6620RegTargetTachMSB+2*ch, tMSB); err != nil {
				refused++
				continue
			}
			if err := b.WriteReg(addr, max6620RegTargetTachLSB+2*ch, tLSB); err != nil {
				refused++
			}
		}
	}
	if wrote == 0 {
		return 0, fmt.Errorf("no max6620 channels configured")
	}
	if refused == wrote {
		return refused, fmt.Errorf("all %d max6620 fan writes failed", wrote)
	}
	return refused, nil
}
