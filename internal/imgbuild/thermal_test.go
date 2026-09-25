/* SPDX-License-Identifier: Apache-2.0 */
package imgbuild

import (
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

func TestACoolingLoopNeedsFansAndNotOnlySensors(t *testing.T) {
	sensor := []platformhal.SMBusSensor{
		{Name: "board", Part: "lm90", Accel: 0, Bus: 0, Addr: 0x4c},
	}
	fans := &platformhal.FanController{
		Part: "crow-cpld", Accel: 0, Bus: 0, Addr: 0x60, Count: 4, MaxPWM: 255,
	}

	for _, tc := range []struct {
		name string
		smb  *platformhal.SMBusMap
		want bool
	}{
		// The 7150S-52's state while its fans are still unfound. This is the
		// case that used to start a regulator with nothing to drive.
		{"sensors but no fans", &platformhal.SMBusMap{Sensors: sensor}, false},
		{"neither", &platformhal.SMBusMap{}, false},
		{"fans but no sensors", &platformhal.SMBusMap{Fans: fans}, false},
		{"both", &platformhal.SMBusMap{Sensors: sensor, Fans: fans}, true},
	} {
		b := &board.Board{PlatformHAL: board.PlatformHAL{Driver: "scd", SMBus: tc.smb}}
		if got := wantsThermalService(b); got != tc.want {
			t.Errorf("%s: cooling loop = %v, want %v", tc.name, got, tc.want)
		}
	}
}
