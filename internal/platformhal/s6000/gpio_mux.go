/* SPDX-License-Identifier: Apache-2.0 */

package s6000

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// Dell S6000-ON routes its 4-channel I2C mux behind the Atom S1220 SCH SMBus
// adapter ("SMBus SCH adapter at 0400") using two SCH GPIO lines (offsets 1
// and 2 from the sch_gpio base):
//
//	ch 0 (00): CPLDs (0x31..33), EMC1403 (0x4d), JC42 (0x18), board EEPROM (0x53)
//	ch 1 (01): MAX6620 fans (0x29, 0x2a), TMP75 sensors (0x4c..4e), LTC4215
//	ch 2 (10): QSFP+ muxes (ports 1..16)
//	ch 3 (11): QSFP+ muxes (ports 17..32)
const (
	schMuxGPIO0Offset = 1
	schMuxGPIO1Offset = 2
	schMuxFanChannel  = 1
)

func findSCHAdapter(root string) (int, bool) {
	matches, err := filepath.Glob(filepath.Join(root, "bus", "i2c", "devices", "i2c-*"))
	if err != nil {
		return 0, false
	}
	sort.Strings(matches)
	for _, d := range matches {
		b, err := os.ReadFile(filepath.Join(d, "name"))
		if err != nil {
			continue
		}
		if strings.Contains(string(b), "SCH") {
			base := filepath.Base(d)
			if n, err := strconv.Atoi(strings.TrimPrefix(base, "i2c-")); err == nil {
				return n, true
			}
		}
	}
	return 0, false
}

func findSCHGPIOBase(root string) (int, bool) {
	chips, err := filepath.Glob(filepath.Join(root, "class", "gpio", "gpiochip*"))
	if err != nil {
		return 0, false
	}
	sort.Strings(chips)
	for _, c := range chips {
		lbl, err := os.ReadFile(filepath.Join(c, "label"))
		if err != nil {
			continue
		}
		s := strings.TrimSpace(string(lbl))
		if strings.Contains(s, "sch_gpio") || strings.Contains(s, "gpio_sch") {
			if base, err := readInt(filepath.Join(c, "base")); err == nil {
				return base, true
			}
		}
	}
	return 0, false
}

func setGPIOOut(root string, pin, val int) error {
	gpioDir := filepath.Join(root, "class", "gpio", fmt.Sprintf("gpio%d", pin))
	if _, err := os.Stat(gpioDir); os.IsNotExist(err) {
		exportPath := filepath.Join(root, "class", "gpio", "export")
		if err := os.WriteFile(exportPath, []byte(fmt.Sprintf("%d\n", pin)), 0o200); err != nil {
			return fmt.Errorf("export gpio%d: %w", pin, err)
		}
	}
	if err := os.WriteFile(filepath.Join(gpioDir, "direction"), []byte("out\n"), 0o644); err != nil {
		return fmt.Errorf("gpio%d direction: %w", pin, err)
	}
	v := "0\n"
	if val != 0 {
		v = "1\n"
	}
	if err := os.WriteFile(filepath.Join(gpioDir, "value"), []byte(v), 0o644); err != nil {
		return fmt.Errorf("gpio%d value: %w", pin, err)
	}
	return nil
}

// selectSCHMuxChannel switches the S6000-ON's 4-channel GPIO I2C mux to ch
// (0..3) and returns the underlying SCH SMBus adapter number.
func selectSCHMuxChannel(root string, ch int) (int, error) {
	bus, ok := findSCHAdapter(root)
	if !ok {
		return 0, fmt.Errorf("no SCH SMBus adapter in %s/bus/i2c/devices", root)
	}
	base, ok := findSCHGPIOBase(root)
	if !ok {
		return 0, fmt.Errorf("no sch_gpio controller in %s/class/gpio", root)
	}
	if err := setGPIOOut(root, base+schMuxGPIO0Offset, ch&1); err != nil {
		return 0, err
	}
	if err := setGPIOOut(root, base+schMuxGPIO1Offset, (ch>>1)&1); err != nil {
		return 0, err
	}
	return bus, nil
}
