/* SPDX-License-Identifier: Apache-2.0 */

// Package s6000 is the platform HAL for the Dell Networking S6000-ON.
//
// In-tree hwmon drivers (lm75/tmp75, emc1403, jc42, pmbus) are read through
// /sys/class/hwmon. Parts with no upstream Linux driver — starting with the
// two MAX6620 fan controllers at 0x29/0x2a on mux channel 1 — are driven
// directly over /dev/i2c-* via SMBus byte-data transactions.
package s6000

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

func init() { platformhal.Register("s6000", Open) }

// sysfsRoot is overridden in tests to point at a mock sysfs tree.
var sysfsRoot = "/sys"

const (
	// floorPercent is the minimum fan duty on S6000-ON (40% in platform.json).
	floorPercent = 40
	// fanCount is 3 fan trays x 2 fans per tray = 6 fans.
	fanCount = 6
)

type hal struct {
	root  string
	muxMu sync.Mutex
}

var (
	_ platformhal.HAL     = (*hal)(nil)
	_ platformhal.Cooling = (*hal)(nil)
)

// Open constructs the S6000-ON platform HAL.
func Open(cfg platformhal.Config) (platformhal.HAL, error) {
	return &hal{root: sysfsRoot}, nil
}

func (h *hal) Board() (platformhal.Identity, error) {
	return platformhal.Identity{}, fmt.Errorf("%w: board identity EEPROM not yet wired", platformhal.ErrUnsupported)
}

func (h *hal) ResetState(platformhal.Reset) (bool, error) {
	return false, platformhal.ErrUnsupported
}

func (h *hal) ReleaseSwitchChip(context.Context) error {
	return platformhal.ErrUnsupported
}

func (h *hal) Watchdog() (platformhal.Watchdog, error) {
	return nil, platformhal.ErrUnsupported
}

type hwmonDev struct {
	dir  string
	name string
	addr string // e.g. "11-004c" from device symlink if available
}

func (h *hal) hwmons() ([]hwmonDev, error) {
	all, err := filepath.Glob(filepath.Join(h.root, "class", "hwmon", "hwmon*"))
	if err != nil {
		return nil, err
	}
	sort.Strings(all)
	var out []hwmonDev
	for _, d := range all {
		b, err := os.ReadFile(filepath.Join(d, "name"))
		if err != nil {
			continue
		}
		var addr string
		if target, err := os.Readlink(filepath.Join(d, "device")); err == nil {
			addr = filepath.Base(target)
		}
		out = append(out, hwmonDev{
			dir:  d,
			name: strings.TrimSpace(string(b)),
			addr: addr,
		})
	}
	return out, nil
}

func readInt(path string) (int, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	return strconv.Atoi(strings.TrimSpace(string(b)))
}

func (h *hal) Temperatures() (map[string]int, error) {
	devs, err := h.hwmons()
	if err != nil {
		return nil, err
	}
	temps := make(map[string]int)
	psuIdx := 0
	for _, dev := range devs {
		switch dev.name {
		case "lm75", "tmp75":
			v, err := readInt(filepath.Join(dev.dir, "temp1_input"))
			if err != nil {
				continue
			}
			label := "board-" + dev.addr
			switch {
			case strings.HasSuffix(dev.addr, "-004c"):
				label = "ASIC-On-Board"
			case strings.HasSuffix(dev.addr, "-004d"):
				label = "NIC"
			case strings.HasSuffix(dev.addr, "-004e"):
				label = "System-Front"
			}
			temps[label] = v
		case "emc1403":
			for ch, name := range map[int]string{1: "CPU0", 2: "CPU1", 3: "CPU-DTS"} {
				if v, err := readInt(filepath.Join(dev.dir, fmt.Sprintf("temp%d_input", ch))); err == nil {
					temps[name] = v
				}
			}
		case "jc42":
			if v, err := readInt(filepath.Join(dev.dir, "temp1_input")); err == nil {
				temps["DIMM"] = v
			}
		case "dni_dps460", "pmbus":
			psuIdx++
			for ch := 1; ch <= 2; ch++ {
				if v, err := readInt(filepath.Join(dev.dir, fmt.Sprintf("temp%d_input", ch))); err == nil {
					temps[fmt.Sprintf("PSU%d-Sensor%d", psuIdx, ch)] = v
				}
			}
		}
	}
	if len(temps) == 0 {
		h.readSMBusTemps(temps)
	}
	return temps, nil
}

// readSMBusTemps reads the S6000-ON's TMP75 (mux ch 1: 0x4c, 0x4d, 0x4e) and
// EMC1403 (mux ch 0: 0x4d) sensors directly over the SCH GPIO-muxed SMBus when
// no hwmon drivers are bound.
func (h *hal) readSMBusTemps(temps map[string]int) {
	readC := func(b smbus, addr, reg int) (int, bool) {
		v, err := b.ReadReg(addr, reg)
		if err != nil {
			return 0, false
		}
		c := int(int8(v))
		if c <= 0 || c >= 125 {
			return 0, false
		}
		return c * 1000, true
	}

	h.muxMu.Lock()
	defer h.muxMu.Unlock()

	if busNum, err := selectSCHMuxChannel(h.root, schMuxFanChannel); err == nil {
		if b, err := openSMBus(busNum); err == nil {
			for _, s := range []struct {
				addr int
				name string
			}{
				{0x4c, "ASIC-On-Board"},
				{0x4d, "NIC"},
				{0x4e, "System-Front"},
			} {
				if mv, ok := readC(b, s.addr, 0x00); ok {
					temps[s.name] = mv
				}
			}
			_ = b.Close()
		}
	}

	if busNum, err := selectSCHMuxChannel(h.root, 0); err == nil {
		if b, err := openSMBus(busNum); err == nil {
			for _, s := range []struct {
				reg  int
				name string
			}{
				{0x00, "CPU-DTS"},
				{0x01, "CPU0"},
				{0x23, "CPU1"},
			} {
				if mv, ok := readC(b, 0x4d, s.reg); ok {
					temps[s.name] = mv
				}
			}
			_ = b.Close()
		}
	}
}

func (h *hal) PSUPresent() (map[string]bool, error) {
	devs, err := h.hwmons()
	if err != nil {
		return nil, err
	}
	out := make(map[string]bool)
	idx := 0
	for _, dev := range devs {
		if dev.name != "dni_dps460" && dev.name != "pmbus" {
			continue
		}
		idx++
		name := fmt.Sprintf("PSU%d", idx)
		if v, err := readInt(filepath.Join(dev.dir, "in1_input")); err == nil && v > 0 {
			out[name] = true
		} else {
			out[name] = false
		}
	}
	return out, nil
}

// max6620 has 4 channels per chip, or 6 fans across channels 1,2,5,6 / two chips.
// Read all fan*_input and pwm* attributes present on max6620 hwmon devices.
func (h *hal) max6620Devs() ([]hwmonDev, error) {
	devs, err := h.hwmons()
	if err != nil {
		return nil, err
	}
	var out []hwmonDev
	for _, d := range devs {
		if d.name == "max6620" {
			out = append(out, d)
		}
	}
	return out, nil
}

// openFanBus returns an SMBus handle routed to mux channel 1 (MAX6620 fan
// controllers at 0x29 and 0x2a) and a cleanup function that closes the bus
// and releases the mux lock.
//
// Resolution order:
//  1. Kernel i2c-mux-gpio channel-1 symlink (if bound).
//  2. Direct SCH GPIO mux selection (sch_gpio base+1/base+2 -> SMBus SCH).
//  3. Adapter owning lm75/tmp75 at 0x4c, or fallback bus 11.
func (h *hal) openFanBus() (smbus, func(), error) {
	if matches, err := filepath.Glob(filepath.Join(h.root, "devices", "platform", "i2c-mux-gpio*", "channel-1")); err == nil {
		for _, m := range matches {
			if target, err := os.Readlink(m); err == nil {
				base := filepath.Base(target)
				if strings.HasPrefix(base, "i2c-") {
					if n, err := strconv.Atoi(strings.TrimPrefix(base, "i2c-")); err == nil {
						b, err := openSMBus(n)
						if err != nil {
							return nil, nil, err
						}
						return b, func() { _ = b.Close() }, nil
					}
				}
			}
		}
	}

	h.muxMu.Lock()
	if busNum, err := selectSCHMuxChannel(h.root, schMuxFanChannel); err == nil {
		b, err := openSMBus(busNum)
		if err != nil {
			h.muxMu.Unlock()
			return nil, nil, err
		}
		return b, func() {
			_ = b.Close()
			h.muxMu.Unlock()
		}, nil
	}
	h.muxMu.Unlock()

	busNum := 11
	if devs, err := h.hwmons(); err == nil {
		for _, dev := range devs {
			if (dev.name == "lm75" || dev.name == "tmp75") && strings.HasSuffix(dev.addr, "-004c") {
				if busStr, _, ok := strings.Cut(dev.addr, "-"); ok {
					if n, err := strconv.Atoi(busStr); err == nil {
						busNum = n
						break
					}
				}
			}
		}
	}
	b, err := openSMBus(busNum)
	if err != nil {
		return nil, nil, err
	}
	return b, func() { _ = b.Close() }, nil
}

func (h *hal) Fans() ([]platformhal.Fan, error) {
	devs, err := h.max6620Devs()
	if err != nil {
		return nil, err
	}
	if len(devs) == 0 {
		b, done, err := h.openFanBus()
		if err != nil {
			return nil, fmt.Errorf("%w: no max6620 hwmon and %v", platformhal.ErrUnsupported, err)
		}
		defer done()
		return readMax6620Fans(b)
	}
	var fans []platformhal.Fan
	idx := 0
	for _, dev := range devs {
		inputs, _ := filepath.Glob(filepath.Join(dev.dir, "fan*_input"))
		sort.Strings(inputs)
		for _, p := range inputs {
			rpm, err := readInt(p)
			if err != nil {
				continue
			}
			idx++
			base := filepath.Base(p)
			ch := strings.TrimSuffix(strings.TrimPrefix(base, "fan"), "_input")
			pct := 0
			rawPWM := ""
			if pwm, err := readInt(filepath.Join(dev.dir, "pwm"+ch)); err == nil {
				pct = (pwm*100 + 127) / 255
				rawPWM = fmt.Sprintf("pwm%s=%d", ch, pwm)
			} else if pwm1, err := readInt(filepath.Join(dev.dir, "pwm1")); err == nil {
				pct = (pwm1*100 + 127) / 255
				rawPWM = fmt.Sprintf("pwm1=%d", pwm1)
			}
			fans = append(fans, platformhal.Fan{
				Index:   idx,
				Present: rpm > 0,
				RPM:     rpm,
				Percent: pct,
				Raw:     rawPWM,
			})
		}
	}
	return fans, nil
}

func (h *hal) FanFloorPercent() int { return floorPercent }
func (h *hal) FanCount() int        { return fanCount }

func (h *hal) SetFanPercent(pct int) (int, error) {
	if pct < floorPercent {
		pct = floorPercent
	}
	if pct > 100 {
		pct = 100
	}
	devs, err := h.max6620Devs()
	if err != nil {
		return 0, err
	}
	if len(devs) == 0 {
		b, done, err := h.openFanBus()
		if err != nil {
			return 0, fmt.Errorf("%w: no max6620 hwmon and %v", platformhal.ErrUnsupported, err)
		}
		defer done()
		return writeMax6620Percent(b, pct)
	}
	pwmVal := (pct*255 + 50) / 100
	refused := 0
	wrote := 0
	for _, dev := range devs {
		pwms, _ := filepath.Glob(filepath.Join(dev.dir, "pwm[0-9]"))
		sort.Strings(pwms)
		for _, p := range pwms {
			wrote++
			if err := os.WriteFile(p, []byte(fmt.Sprintf("%d\n", pwmVal)), 0o644); err != nil {
				refused++
			}
		}
	}
	if wrote == 0 {
		return 0, fmt.Errorf("no pwm attributes found on max6620")
	}
	return refused, nil
}
