/* SPDX-License-Identifier: Apache-2.0 */
package n3172tq

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

func TestSatisfiesTheHALAndCooling(t *testing.T) {
	var h any = &hal{}
	if _, ok := h.(platformhal.HAL); !ok {
		t.Error("does not satisfy HAL")
	}
	if _, ok := h.(platformhal.Cooling); !ok {
		t.Error("does not satisfy Cooling")
	}
}

func ch(n int) *int { return &n }

// fakeSys builds the shape /sys has on the Nexus 3172TQ once its i2c devices
// are instantiated: an ADT7462 with three populated temperature channels and
// four bays, and two identical pmbus supplies told apart only by mux channel.
func fakeSys(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	write := func(p, v string) {
		t.Helper()
		full := filepath.Join(root, p)
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(v+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	link := func(from, to string) {
		t.Helper()
		full := filepath.Join(root, from)
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(to, full); err != nil {
			t.Fatal(err)
		}
	}

	// The CPU zones are present too, and must not be mistaken for the board's.
	write("class/hwmon/hwmon0/name", "acpitz")
	write("class/hwmon/hwmon0/temp1_input", "27800")

	write("class/hwmon/hwmon1/name", "adt7462")
	write("class/hwmon/hwmon1/temp1_input", "36000")
	write("class/hwmon/hwmon1/temp2_input", "36000")
	write("class/hwmon/hwmon1/temp3_input", "31000")
	for i, rpm := range []string{"7400", "7300", "7350", "0"} {
		write(filepath.Join("class/hwmon/hwmon1", fmt.Sprintf("fan%d_input", i+1)), rpm)
	}
	write("class/hwmon/hwmon1/pwm1", "40") // 0x28, the vendor's idle duty
	link("class/hwmon/hwmon1/device", "../../../1-0058")

	// Two supplies, same driver, same address, different channel.
	write("class/hwmon/hwmon2/name", "pmbus")
	link("class/hwmon/hwmon2/device", "../../../3-005b")
	write("class/hwmon/hwmon3/name", "pmbus")
	link("class/hwmon/hwmon3/device", "../../../4-005b")

	// The mux, with the channel-N symlinks the kernel creates.
	write("bus/i2c/devices/0-0070/name", "pca9548")
	link("bus/i2c/devices/0-0070/channel-2", "../i2c-3")
	link("bus/i2c/devices/0-0070/channel-3", "../i2c-4")
	return root
}

func realConfig() platformhal.Config {
	return platformhal.Config{BoardData: &Data{
		Hwmon: &Map{
			Sensors: []Sensor{
				{Name: "Front-Left", Chip: "adt7462", Channel: 1},
				{Name: "Front-Right", Chip: "adt7462", Channel: 2},
				{Name: "Back", Chip: "adt7462", Channel: 3},
			},
			Fans: &Fans{Chip: "adt7462", Count: 4, FloorPercent: 25},
			PSUs: []PSU{
				{Name: "PSU1", Chip: "pmbus", Address: 0x5b, Channel: ch(2)},
				{Name: "PSU2", Chip: "pmbus", Address: 0x5b, Channel: ch(3)},
			},
		},
		I2C: &KernelI2C{
			Adapter: "SMBus I801 adapter",
			Mux:     &I2CDevice{Driver: "pca9548", Address: 0x70},
		},
	}}
}

func open(t *testing.T, root string) platformhal.HAL {
	t.Helper()
	h, err := Open(realConfig())
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	h.(*hal).root = root
	return h
}

func TestTemperaturesAreNamedByLocation(t *testing.T) {
	h := open(t, fakeSys(t))
	got, err := h.Temperatures()
	if err != nil {
		t.Fatalf("Temperatures: %v", err)
	}
	want := map[string]int{"Front-Left": 36000, "Front-Right": 36000, "Back": 31000}
	if len(got) != len(want) {
		t.Fatalf("got %v, want %v", got, want)
	}
	for k, v := range want {
		if got[k] != v {
			t.Errorf("%s = %d, want %d", k, got[k], v)
		}
	}
	// The CPU zone is on the same bus of hwmon devices and is not the board's.
	if _, ok := got["acpitz"]; ok {
		t.Error("an ACPI zone was reported as a board sensor")
	}
}

// A channel the chip does not populate must be left out, not reported as 0:
// a cooling loop that reads 0 degrees for the hottest part of a board winds
// the fans down.
func TestUnpopulatedChannelIsOmittedNotZero(t *testing.T) {
	cfg := realConfig()
	d := cfg.BoardData.(*Data)
	d.Hwmon.Sensors = append(d.Hwmon.Sensors,
		Sensor{Name: "Nonexistent", Chip: "adt7462", Channel: 8})
	h, err := Open(cfg)
	if err != nil {
		t.Fatal(err)
	}
	h.(*hal).root = fakeSys(t)
	got, err := h.Temperatures()
	if err != nil {
		t.Fatal(err)
	}
	if v, ok := got["Nonexistent"]; ok {
		t.Errorf("an absent channel was reported as %d", v)
	}
}

// Two identical supplies at one address are told apart by mux channel, through
// the channel-N symlink rather than by assuming adapter numbering.
func TestPSUsAreToldApartByMuxChannel(t *testing.T) {
	h := open(t, fakeSys(t))
	got, err := h.PSUPresent()
	if err != nil {
		t.Fatalf("PSUPresent: %v", err)
	}
	for _, n := range []string{"PSU1", "PSU2"} {
		if !got[n] {
			t.Errorf("%s reported absent; it is in the fake tree", n)
		}
	}
	if len(got) != 2 {
		t.Errorf("got %v, want exactly two supplies", got)
	}
}

func TestPSUAbsentWhenItsChannelHasNothing(t *testing.T) {
	root := fakeSys(t)
	// Pull PSU2's hwmon out from under it, leaving the mux channel intact.
	if err := os.RemoveAll(filepath.Join(root, "class/hwmon/hwmon3")); err != nil {
		t.Fatal(err)
	}
	h := open(t, root)
	got, err := h.PSUPresent()
	if err != nil {
		t.Fatal(err)
	}
	if got["PSU2"] {
		t.Error("PSU2 reported present with no hwmon behind it")
	}
	if !got["PSU1"] {
		t.Error("PSU1 went absent when PSU2 did")
	}
}

func TestFansReportRPMAndDutyAndSpotADeadBay(t *testing.T) {
	h := open(t, fakeSys(t))
	c, ok := h.(platformhal.Cooling)
	if !ok {
		t.Fatal("not Cooling")
	}
	fans, err := c.Fans()
	if err != nil {
		t.Fatalf("Fans: %v", err)
	}
	if len(fans) != 4 {
		t.Fatalf("got %d bays, want 4", len(fans))
	}
	if !fans[0].Present || fans[0].RPM != 7400 {
		t.Errorf("bay 1: %+v", fans[0])
	}
	// 40/255 rounds to 16%.
	if fans[0].Percent != 16 {
		t.Errorf("bay 1 duty = %d%%, want 16%% (pwm 40 of 255)", fans[0].Percent)
	}
	// The fourth reads 0 RPM, which is a dead or empty bay and must be visible.
	if fans[3].Present {
		t.Error("a bay reading 0 RPM was reported present")
	}
}

// A board clamps to its floor rather than obeying a request to stop.
func TestSetFanPercentClampsToTheBoardFloor(t *testing.T) {
	root := fakeSys(t)
	h := open(t, root)
	c := h.(platformhal.Cooling)
	if _, err := c.SetFanPercent(0); err != nil {
		t.Fatalf("SetFanPercent: %v", err)
	}
	raw, err := readInt(filepath.Join(root, "class/hwmon/hwmon1/pwm1"))
	if err != nil {
		t.Fatal(err)
	}
	if want := percentToPWM(25); raw != want {
		t.Errorf("pwm1 = %d after asking for 0%%, want the 25%% floor (%d)", raw, want)
	}
	if c.FanFloorPercent() != 25 || c.FanCount() != 4 {
		t.Errorf("floor=%d count=%d", c.FanFloorPercent(), c.FanCount())
	}
}

// A board with no parts instantiated must say so usefully rather than report
// zero degrees, because "no sensors" is exactly what someone is diagnosing.
func TestNoInstantiatedPartsIsUnsupportedNotZero(t *testing.T) {
	h := open(t, t.TempDir())
	if _, err := h.Temperatures(); !errors.Is(err, platformhal.ErrUnsupported) {
		t.Errorf("Temperatures with an empty tree: %v", err)
	}
	c := h.(platformhal.Cooling)
	if _, err := c.Fans(); !errors.Is(err, platformhal.ErrUnsupported) {
		t.Errorf("Fans with an empty tree: %v", err)
	}
}

// The things this board cannot do must say ErrUnsupported, not pretend.
func TestUnreachableFacilitiesRefuseClearly(t *testing.T) {
	h := open(t, fakeSys(t))
	if _, err := h.Watchdog(); !errors.Is(err, platformhal.ErrUnsupported) {
		t.Errorf("Watchdog: %v", err)
	}
	if err := h.ReleaseSwitchChip(nil); !errors.Is(err, platformhal.ErrUnsupported) {
		t.Errorf("ReleaseSwitchChip: %v", err)
	}
	if _, err := h.Board(); !errors.Is(err, platformhal.ErrUnsupported) {
		t.Errorf("Board: %v", err)
	}
}

// A board that names no hwmon map has no business opening this driver.
func TestOpenRefusesWithoutAMap(t *testing.T) {
	if _, err := Open(platformhal.Config{}); !errors.Is(err, platformhal.ErrUnsupported) {
		t.Errorf("Open with no board data: %v", err)
	}
	// Another board's data must be refused by name, not silently half-work.
	if _, err := Open(platformhal.Config{BoardData: &platformhal.SMBusMap{}}); err == nil {
		t.Error("Open accepted another board's platform data")
	}
}
