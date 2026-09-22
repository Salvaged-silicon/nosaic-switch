/* SPDX-License-Identifier: Apache-2.0 */
package imgbuild

import (
	"os/exec"
	"strings"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal/n3172tq"
)

func ch(n int) *int { return &n }

func realBoard() *board.Board {
	return &board.Board{
		ID: "cisco-n3172tq",
		PlatformHAL: board.PlatformHAL{N3172TQ: &n3172tq.Data{I2C: &n3172tq.KernelI2C{
			Adapter: "SMBus I801 adapter",
			Mux:     &n3172tq.I2CDevice{Driver: "pca9548", Address: 0x70},
			Devices: []n3172tq.I2CDevice{
				{Driver: "adt7462", Address: 0x58, Channel: ch(0), Note: "temps and fans"},
				{Driver: "24c512", Address: 0x52, Channel: ch(0)},
				{Driver: "pmbus", Address: 0x5b, Channel: ch(2)},
				{Driver: "pmbus", Address: 0x5b, Channel: ch(3)},
			},
		}}},
	}
}

// The generated script has to be valid shell. A script that is only run on a
// switch is a script nobody runs until it matters.
func TestI2CScriptIsValidShell(t *testing.T) {
	s := i2cDevicesScript(realBoard())
	if s == "" {
		t.Fatal("no script for a board that declares i2c devices")
	}
	cmd := exec.Command("sh", "-n")
	cmd.Stdin = strings.NewReader(s)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("sh -n rejected the generated script: %v\n%s\n---\n%s", err, out, s)
	}
}

// Addresses are written as two hex digits with no 0x, because that is what
// new_device wants and what the sysfs name of an instantiated device uses.
// 0x58 rendered as "88" or as "0x58" both break, differently.
func TestI2CScriptRendersAddressesAsSysfsHex(t *testing.T) {
	s := i2cDevicesScript(realBoard())
	for _, want := range []string{`new "$parent" 70 "pca9548"`, `58 "adt7462"`, `5b "pmbus"`} {
		if !strings.Contains(s, want) {
			t.Errorf("script is missing %q", want)
		}
	}
	if strings.Contains(s, "0x58 \"adt7462\"") {
		t.Error("address rendered with a 0x prefix; new_device takes the value, the shell adds 0x")
	}
}

// A mux channel is resolved through the mux's own channel-N symlink, not by
// assuming the child adapters are numbered parent+1+N. The arithmetic happens
// to be right on a freshly booted board and is not promised anywhere.
func TestI2CScriptResolvesChannelsThroughSysfs(t *testing.T) {
	s := i2cDevicesScript(realBoard())
	if !strings.Contains(s, `channel-$1`) {
		t.Error("channels are not resolved through the mux's channel-N symlink")
	}
	if strings.Contains(s, "parent + 1") || strings.Contains(s, "$((parent") {
		t.Error("script does arithmetic on adapter numbers")
	}
}

// A board with nothing declared gets no script and therefore no service, rather
// than a service that succeeds at doing nothing.
func TestI2CScriptEmptyWithoutDeclaration(t *testing.T) {
	if s := i2cDevicesScript(&board.Board{ID: "x"}); s != "" {
		t.Errorf("script generated for a board with no i2c block:\n%s", s)
	}
	empty := &board.Board{ID: "x", PlatformHAL: board.PlatformHAL{
		N3172TQ: &n3172tq.Data{I2C: &n3172tq.KernelI2C{Adapter: "a"}}}}
	if s := i2cDevicesScript(empty); s != "" {
		t.Errorf("script generated for an empty device list:\n%s", s)
	}
}

// Every shipped board's i2c block has to generate a script the shell accepts.
// A board file is data, and data that renders to broken shell fails on the
// switch rather than here unless something checks it here.
func TestI2CScriptForEveryShippedBoard(t *testing.T) {
	boards, err := board.LoadAll("../..")
	if err != nil {
		t.Fatalf("loading boards: %v", err)
	}
	seen := 0
	for _, b := range boards {
		s := i2cDevicesScript(b)
		if s == "" {
			continue
		}
		seen++
		cmd := exec.Command("sh", "-n")
		cmd.Stdin = strings.NewReader(s)
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Errorf("%s: generated script is not valid shell: %v\n%s", b.ID, err, out)
		}
		t.Logf("%s:\n%s", b.ID, s)
	}
	if seen == 0 {
		t.Skip("no shipped board declares i2c devices yet")
	}
}
