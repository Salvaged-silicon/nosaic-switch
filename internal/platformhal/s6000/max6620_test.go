/* SPDX-License-Identifier: Apache-2.0 */

package s6000

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

type mockSMBus struct {
	regs map[int]map[int]byte // addr -> reg -> val
}

func newMockSMBus() *mockSMBus {
	return &mockSMBus{regs: map[int]map[int]byte{}}
}

func (m *mockSMBus) set(addr, reg int, v byte) {
	if m.regs[addr] == nil {
		m.regs[addr] = map[int]byte{}
	}
	m.regs[addr][reg] = v
}

func (m *mockSMBus) ReadReg(addr, reg int) (byte, error) {
	dev, ok := m.regs[addr]
	if !ok {
		return 0, fmt.Errorf("no device at %#02x", addr)
	}
	return dev[reg], nil
}

func (m *mockSMBus) WriteReg(addr, reg int, v byte) error {
	dev, ok := m.regs[addr]
	if !ok {
		return fmt.Errorf("no device at %#02x", addr)
	}
	dev[reg] = v
	return nil
}

func (m *mockSMBus) Close() error { return nil }

func TestMax6620DecodeRPM(t *testing.T) {
	// SR = 4 (cfg bits 6..5 = 10 -> 0x40), count = 115 -> (245760 * 4) / 115 = 8548 RPM.
	count := uint16(115)
	msb := byte(count >> 3)
	lsb := byte((count & 0x7) << 5)
	got := max6620DecodeRPM(msb, lsb, 0x40)
	if got != 8548 {
		t.Errorf("max6620DecodeRPM = %d, want 8548", got)
	}

	// Saturated (0x7ff) or zero count means stalled/absent (0 RPM).
	if got := max6620DecodeRPM(0xff, 0xe0, 0x40); got != 0 {
		t.Errorf("saturated tach decoded as %d, want 0", got)
	}
	if got := max6620DecodeRPM(0x00, 0x00, 0x40); got != 0 {
		t.Errorf("zero tach decoded as %d, want 0", got)
	}
}

func TestMax6620UserspaceI2C(t *testing.T) {
	// Empty hwmon so HAL falls back to userspace MAX6620 over SMBus.
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "class", "hwmon"), 0o755); err != nil {
		t.Fatal(err)
	}
	// Expose a mock i2c-mux-gpio channel-1 -> i2c-11 symlink.
	muxDir := filepath.Join(root, "devices", "platform", "i2c-mux-gpio.0")
	busDir := filepath.Join(root, "bus", "i2c", "devices", "i2c-11")
	if err := os.MkdirAll(muxDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(busDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(busDir, filepath.Join(muxDir, "channel-1")); err != nil {
		t.Fatal(err)
	}

	mb := newMockSMBus()
	for _, chip := range max6620Chips {
		addr := chip.addr
		mb.set(addr, max6620RegGlobalCfg, 0x10)
		for ch := 0; ch < chip.channels; ch++ {
			// Start in DAC mode (bit 7 = 0) with SR = 2 (0x28) to verify
			// SetFanPercent enables RPM_MODE (bit 7 = 1).
			mb.set(addr, max6620RegFanCfg0+ch, 0x28)
			// SR = 2, count = 49 -> 10031 RPM
			mb.set(addr, max6620RegTachMSB0+2*ch, byte(49>>3))
			mb.set(addr, max6620RegTachLSB0+2*ch, byte((49&0x7)<<5))
			// 50% DAC (256 = 0x80 << 1)
			mb.set(addr, max6620RegDACMSB0+2*ch, 0x80)
			mb.set(addr, max6620RegDACLSB0+2*ch, 0x00)
		}
	}

	oldRoot := sysfsRoot
	oldOpen := openSMBus
	sysfsRoot = root
	gotBus := -1
	openSMBus = func(n int) (smbus, error) {
		gotBus = n
		return mb, nil
	}
	defer func() {
		sysfsRoot = oldRoot
		openSMBus = oldOpen
	}()

	h, err := platformhal.Open("s6000", platformhal.Config{})
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	cooling := h.(platformhal.Cooling)

	fans, err := cooling.Fans()
	if err != nil {
		t.Fatalf("Fans: %v", err)
	}
	if gotBus != 11 {
		t.Errorf("opened i2c bus %d, want 11 from i2c-mux-gpio channel-1", gotBus)
	}
	if len(fans) != 6 {
		t.Fatalf("len(fans) = %d, want 6", len(fans))
	}
	if fans[0].RPM != 10031 || !fans[0].Present || fans[0].Percent != 50 {
		t.Errorf("fans[0] = %+v, want RPM=10031 Present=true Percent=50", fans[0])
	}

	// Below floorPercent (40%) must clamp to 40% (8000 RPM -> count 61) and set RPM_MODE bit.
	refused, err := cooling.SetFanPercent(10)
	if err != nil || refused != 0 {
		t.Fatalf("SetFanPercent(10): refused=%d err=%v", refused, err)
	}
	if cfg := mb.regs[0x29][max6620RegFanCfg0]; cfg&max6620FanRPMModeBit == 0 {
		t.Errorf("RPM_MODE bit not set after SetFanPercent: %#02x", cfg)
	}
	gotMSB := mb.regs[0x29][max6620RegTargetTachMSB]
	gotLSB := mb.regs[0x29][max6620RegTargetTachLSB]
	gotCount := (int(gotMSB) << 3) | (int(gotLSB) >> 5)
	if gotCount != 61 {
		t.Errorf("target tach count at clamped 40%% = %d, want 61 (8000 RPM)", gotCount)
	}
}

func TestSCHGPIOMuxSelection(t *testing.T) {
	root := t.TempDir()
	// Mock i2c-0 = SMBus SCH adapter at 0400
	i2c0 := filepath.Join(root, "bus", "i2c", "devices", "i2c-0")
	if err := os.MkdirAll(i2c0, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(i2c0, "name"), []byte("SMBus SCH adapter at 0400\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	// Mock sch_gpio chip at base 512 with pre-exported gpio513 and gpio514
	chip := filepath.Join(root, "class", "gpio", "gpiochip512")
	if err := os.MkdirAll(chip, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(chip, "label"), []byte("sch_gpio\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(chip, "base"), []byte("512\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	for _, pin := range []int{513, 514} {
		dir := filepath.Join(root, "class", "gpio", fmt.Sprintf("gpio%d", pin))
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
	}

	busNum, err := selectSCHMuxChannel(root, schMuxFanChannel)
	if err != nil {
		t.Fatalf("selectSCHMuxChannel: %v", err)
	}
	if busNum != 0 {
		t.Errorf("busNum = %d, want 0", busNum)
	}
	v0, _ := os.ReadFile(filepath.Join(root, "class", "gpio", "gpio513", "value"))
	v1, _ := os.ReadFile(filepath.Join(root, "class", "gpio", "gpio514", "value"))
	if string(v0) != "1\n" || string(v1) != "0\n" {
		t.Errorf("mux gpios = (%q, %q), want (\"1\\n\", \"0\\n\") for channel 1", string(v0), string(v1))
	}
}

func TestS6000SMBusTemperatures(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "class", "hwmon"), 0o755); err != nil {
		t.Fatal(err)
	}
	i2c0 := filepath.Join(root, "bus", "i2c", "devices", "i2c-0")
	if err := os.MkdirAll(i2c0, 0o755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(filepath.Join(i2c0, "name"), []byte("SMBus SCH adapter at 0400\n"), 0o644)
	chip := filepath.Join(root, "class", "gpio", "gpiochip0")
	if err := os.MkdirAll(chip, 0o755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(filepath.Join(chip, "label"), []byte("sch_gpio.3168\n"), 0o644)
	_ = os.WriteFile(filepath.Join(chip, "base"), []byte("0\n"), 0o644)
	for _, pin := range []int{1, 2} {
		_ = os.MkdirAll(filepath.Join(root, "class", "gpio", fmt.Sprintf("gpio%d", pin)), 0o755)
	}

	mb := newMockSMBus()
	mb.set(0x4c, 0x00, 44) // ASIC-On-Board
	mb.set(0x4d, 0x00, 39) // NIC (ch1) / CPU-DTS (ch0)
	mb.set(0x4e, 0x00, 32) // System-Front
	mb.set(0x4d, 0x01, 46) // CPU0
	mb.set(0x4d, 0x23, 47) // CPU1

	oldRoot, oldOpen := sysfsRoot, openSMBus
	sysfsRoot = root
	openSMBus = func(int) (smbus, error) { return mb, nil }
	defer func() {
		sysfsRoot, openSMBus = oldRoot, oldOpen
	}()

	h, err := platformhal.Open("s6000", platformhal.Config{})
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	temps, err := h.Temperatures()
	if err != nil {
		t.Fatalf("Temperatures: %v", err)
	}
	for k, want := range map[string]int{
		"ASIC-On-Board": 44000,
		"NIC":           39000,
		"System-Front":  32000,
		"CPU-DTS":       39000,
		"CPU0":          46000,
		"CPU1":          47000,
	} {
		if got := temps[k]; got != want {
			t.Errorf("temp %s = %d, want %d", k, got, want)
		}
	}
}
