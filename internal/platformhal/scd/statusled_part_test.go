// SPDX-License-Identifier: Apache-2.0

package scd

import (
	"errors"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// A board that has not found the part its lamps live on must not be told to
// run the generator that reads that part.
//
// The generator assumes a fan CPLD at 0x60 on accelerator 0 bus 0. The
// 7150S-52 has no such part -- a full read-only scan of both accelerators
// finds sixteen devices and no fan controller -- so the advice is not merely
// unhelpful there, it sends an operator off to do something that cannot
// work.
func TestLampAdviceDependsOnTheBoardHavingThePart(t *testing.T) {
	t.Setenv("NOSAIC_STATUSLEDS", "/nonexistent/statusleds.conf")

	noFans := &SCD{smbusMap: &platformhal.SMBusMap{}}
	if _, err := noFans.loadLamps(); !errors.Is(err, ErrNoLampPart) {
		t.Errorf("a board with no fan controller should be told the part is\n"+
			"unidentified, not told to run the generator; got: %v", err)
	}

	withFans := &SCD{smbusMap: &platformhal.SMBusMap{
		Fans: &platformhal.FanController{},
	}}
	if _, err := withFans.loadLamps(); !errors.Is(err, ErrNoLampMap) {
		t.Errorf("a board that has the part but no map should be pointed at the\n"+
			"generator; got: %v", err)
	}
}
