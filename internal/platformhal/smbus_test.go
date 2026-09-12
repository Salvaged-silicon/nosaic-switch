package platformhal

import (
	"strings"
	"testing"
)

func goodMap() *SMBusMap {
	return &SMBusMap{
		Sensors: []SMBusSensor{
			{Name: "board", Part: "max6658", SMBusAddr: SMBusAddr{Accel: 0, Bus: 0, Addr: 0x4c}},
			{Name: "front-panel", Part: "max6658-remote", SMBusAddr: SMBusAddr{Accel: 0, Bus: 0, Addr: 0x4c}},
		},
		Fans: &FanController{
			Part: "crow-cpld", SMBusAddr: SMBusAddr{Accel: 0, Bus: 1, Addr: 0x60},
			Count: 4, MaxPWM: 180, FloorPercent: 60,
		},
	}
}

func TestSMBusMapAcceptsARealBoard(t *testing.T) {
	if err := goodMap().Validate(); err != nil {
		t.Fatalf("a valid map was rejected: %v", err)
	}
	// A board with no SMBus devices is a board, not an error.
	if err := (*SMBusMap)(nil).Validate(); err != nil {
		t.Fatalf("a board with no smbus section was rejected: %v", err)
	}
}

func TestSMBusMapRejectsWhatCannotBeRight(t *testing.T) {
	cases := []struct {
		name string
		edit func(*SMBusMap)
		want string
	}{
		{"an address outside the 7-bit range", func(m *SMBusMap) { m.Sensors[0].Addr = 0x100 }, "7-bit"},
		{"a part nothing can decode", func(m *SMBusMap) { m.Sensors[0].Part = "max6659" }, "unknown part"},
		{"the same sensor name twice", func(m *SMBusMap) { m.Sensors[1].Name = "board" }, "twice"},
		{"a sensor with no name", func(m *SMBusMap) { m.Sensors[0].Name = "" }, "no name"},
		{"a full scale of zero", func(m *SMBusMap) { m.Fans.MaxPWM = 0 }, "max_pwm"},
		// The one that matters most: a floor left unstated must not become a
		// floor of zero, because a floor of zero is permission to stop the
		// only fans in the chassis.
		{"an unstated fan floor", func(m *SMBusMap) { m.Fans.FloorPercent = 0 }, "floor_percent"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			m := goodMap()
			c.edit(m)
			err := m.Validate()
			if err == nil {
				t.Fatalf("accepted %s", c.name)
			}
			if !strings.Contains(err.Error(), c.want) {
				t.Fatalf("error %q does not mention %q, so it does not say what to fix", err, c.want)
			}
		})
	}
}

// Every part a board may name must have a register, or Temperatures reaches a
// sensor it cannot decode after validation has already passed it.
func TestEverySMBusPartHasARegister(t *testing.T) {
	for part, reg := range SMBusParts {
		if part == "" {
			t.Error("an unnamed part is in the table")
		}
		if reg < 0 || reg > 0xff {
			t.Errorf("part %q has register %#x, which is not an SMBus register", part, reg)
		}
	}
}
