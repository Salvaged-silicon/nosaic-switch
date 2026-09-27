package main

import (
	"bytes"
	"fmt"
	"strings"
	"testing"
)

// fakeSMBus answers from a map, and can be made to fail a fixed number of
// times per address first -- which is what this bus does in reality.
type fakeSMBus struct {
	devices map[[3]int]byte // accel, bus, addr -> register 0
	flaky   map[[3]int]int  // how many reads to fail before answering
	reads   int
}

func (f *fakeSMBus) SMBusReadReg(accel, bus, addr, reg int) (byte, error) {
	f.reads++
	k := [3]int{accel, bus, addr}
	if n := f.flaky[k]; n > 0 {
		f.flaky[k] = n - 1
		return 0, fmt.Errorf("timeout")
	}
	v, ok := f.devices[k]
	if !ok {
		// An address with nothing on it: the line is not pulled down, so it
		// reads as all ones rather than failing.
		return 0xff, nil
	}
	return v, nil
}

// An all-ones read is an empty bus, not a device.
//
// Without this rule the scan reports every address on both accelerators --
// 1120 of them on the real board, every one 0xff -- which is not a finding,
// it is the absence of one printed 1120 times.
func TestScanTreatsAllOnesAsSilence(t *testing.T) {
	f := &fakeSMBus{devices: map[[3]int]byte{{0, 0, 0x4c}: 0x1d}}
	var out bytes.Buffer
	if err := smbusScanTo(&out, f, []string{"0", "0"}); err != nil {
		t.Fatal(err)
	}
	got := out.String()
	if !strings.Contains(got, "0x4c") {
		t.Errorf("the one real device was not reported:\n%s", got)
	}
	if !strings.Contains(got, "1 device(s)") {
		t.Errorf("want exactly one device, got:\n%s", got)
	}
	if strings.Count(got, "reg0 =") != 1 {
		t.Errorf("an empty address was reported as a device:\n%s", got)
	}
}

// A device that misses a read is still a device.
//
// This bus times out often enough that a part which is definitely present
// fails individual reads, repeatably. A single-shot scan therefore reports
// present parts as absent -- and "no fan controller on this board" was
// concluded from exactly that kind of scan.
func TestScanRetriesBeforeCallingAnAddressSilent(t *testing.T) {
	f := &fakeSMBus{
		devices: map[[3]int]byte{{0, 1, 0x60}: 0x42},
		flaky:   map[[3]int]int{{0, 1, 0x60}: 2}, // answers on the third try
	}
	var out bytes.Buffer
	if err := smbusScanTo(&out, f, []string{"0", "1"}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "0x60") {
		t.Errorf("a device that needed a retry was reported as absent, which is\n"+
			"how a fan controller goes missing:\n%s", out.String())
	}
}

// And a genuinely absent address stays absent however many times it is asked.
func TestScanDoesNotInventDevices(t *testing.T) {
	f := &fakeSMBus{devices: map[[3]int]byte{}}
	var out bytes.Buffer
	if err := smbusScanTo(&out, f, []string{"0", "0"}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "0 device(s)") {
		t.Errorf("an empty bus produced devices:\n%s", out.String())
	}
	if !strings.Contains(out.String(), "not fitted") {
		t.Error("an empty scan should say that silence here is about this board\n" +
			"in this state, not about the board")
	}
}
