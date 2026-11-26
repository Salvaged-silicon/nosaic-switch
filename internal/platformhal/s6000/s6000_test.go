package s6000

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

func setupMockSysfs(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	hwmon := filepath.Join(root, "class", "hwmon")

	addHwmon := func(idx int, name, devName string, files map[string]string) {
		dir := filepath.Join(hwmon, filepath.Base(filepath.Join(hwmon, "hwmon"+string(rune('0'+idx)))))
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "name"), []byte(name+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
		if devName != "" {
			target := filepath.Join(root, "devices", devName)
			if err := os.MkdirAll(target, 0o755); err != nil {
				t.Fatal(err)
			}
			if err := os.Symlink(target, filepath.Join(dir, "device")); err != nil {
				t.Fatal(err)
			}
		}
		for k, v := range files {
			if err := os.WriteFile(filepath.Join(dir, k), []byte(v), 0o644); err != nil {
				t.Fatal(err)
			}
		}
	}

	addHwmon(0, "lm75", "11-004c", map[string]string{"temp1_input": "42000\n"})
	addHwmon(1, "lm75", "11-004d", map[string]string{"temp1_input": "38000\n"})
	addHwmon(2, "lm75", "11-004e", map[string]string{"temp1_input": "31000\n"})
	addHwmon(3, "emc1403", "10-004d", map[string]string{
		"temp1_input": "45000\n",
		"temp2_input": "46000\n",
		"temp3_input": "48000\n",
	})
	addHwmon(4, "jc42", "0-0018", map[string]string{"temp1_input": "35000\n"})
	addHwmon(5, "max6620", "12-002a", map[string]string{
		"fan1_input": "8500\n",
		"fan2_input": "8600\n",
		"pwm1":       "128\n",
		"pwm2":       "128\n",
	})
	addHwmon(6, "dni_dps460", "13-0058", map[string]string{
		"in1_input":   "120000\n",
		"temp1_input": "33000\n",
	})
	addHwmon(7, "dni_dps460", "14-0059", map[string]string{
		"in1_input": "0\n",
	})

	return root
}

func TestS6000HAL(t *testing.T) {
	old := sysfsRoot
	sysfsRoot = setupMockSysfs(t)
	defer func() { sysfsRoot = old }()

	h, err := platformhal.Open("s6000", platformhal.Config{})
	if err != nil {
		t.Fatalf("Open: %v", err)
	}

	temps, err := h.Temperatures()
	if err != nil {
		t.Fatalf("Temperatures: %v", err)
	}
	wantTemps := map[string]int{
		"ASIC-On-Board": 42000,
		"NIC":           38000,
		"System-Front":  31000,
		"CPU0":          45000,
		"CPU1":          46000,
		"CPU-DTS":       48000,
		"DIMM":          35000,
		"PSU1-Sensor1":  33000,
	}
	for k, want := range wantTemps {
		if got := temps[k]; got != want {
			t.Errorf("temp %s = %d, want %d", k, got, want)
		}
	}

	psus, err := h.PSUPresent()
	if err != nil {
		t.Fatalf("PSUPresent: %v", err)
	}
	if !psus["PSU1"] || psus["PSU2"] {
		t.Errorf("PSUPresent = %v, want PSU1=true PSU2=false", psus)
	}

	cooling, ok := h.(platformhal.Cooling)
	if !ok {
		t.Fatal("s6000 HAL must implement platformhal.Cooling")
	}
	fans, err := cooling.Fans()
	if err != nil {
		t.Fatalf("Fans: %v", err)
	}
	if len(fans) != 2 || fans[0].RPM != 8500 || fans[0].Percent != 50 {
		t.Errorf("unexpected fans: %+v", fans)
	}

	// Below floorPercent (40%) must clamp to 40%.
	refused, err := cooling.SetFanPercent(10)
	if err != nil || refused != 0 {
		t.Fatalf("SetFanPercent(10): refused=%d err=%v", refused, err)
	}
	fans, err = cooling.Fans()
	if err != nil {
		t.Fatalf("Fans after set: %v", err)
	}
	if fans[0].Percent != 40 {
		t.Errorf("fan duty = %d%%, want clamped 40%%", fans[0].Percent)
	}
}
