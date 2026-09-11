package sff

import (
	"math"
	"testing"
)

// The conversions are the whole value of this package, and every one of them
// is a fixed-point unit a vendor datasheet states rather than something that
// can be derived. They are pinned against worked values from SFF-8636 and
// SFF-8472 so a plausible-looking mistake -- a factor of ten, a sign -- fails
// here rather than on a switch where nobody has a reference reading.

func TestQSFPTemperatureIsSignedSixteenthsOfADegree(t *testing.T) {
	for _, tc := range []struct {
		name   string
		raw    uint16
		wantMC int
	}{
		{"zero", 0x0000, 0},
		{"25 C", 0x1900, 25000},
		{"half a degree", 0x0080, 500},
		// Below freezing is the case a naive unsigned read gets wrong, and it
		// reads as +231 C rather than as an obviously bad number.
		{"minus 25 C", 0xE700, -25000},
		{"minus half a degree", 0xFF80, -500},
	} {
		t.Run(tc.name, func(t *testing.T) {
			lower := make([]byte, QSFPLowerLen)
			lower[qsfpTemp] = byte(tc.raw >> 8)
			lower[qsfpTemp+1] = byte(tc.raw)
			if got := DecodeQSFP(lower, nil).TempMilliC; got != tc.wantMC {
				t.Errorf("raw %#04x: want %d mC, got %d", tc.raw, tc.wantMC, got)
			}
		})
	}
}

func TestQSFPVoltageIsHundredMicrovoltUnits(t *testing.T) {
	lower := make([]byte, QSFPLowerLen)
	// 3.3 V = 33000 * 100 uV
	lower[qsfpVcc], lower[qsfpVcc+1] = byte(33000>>8), byte(33000&0xff)
	if got := DecodeQSFP(lower, nil).VccMV; got != 3300 {
		t.Errorf("want 3300 mV, got %d", got)
	}
}

func TestQSFPLanePowersAndBias(t *testing.T) {
	lower := make([]byte, QSFPLowerLen)
	// Lane 3 only, to catch an off-by-one in the per-lane stride.
	put := func(off, v int) { lower[off] = byte(v >> 8); lower[off+1] = byte(v) }
	put(qsfpRXPower+2*2, 10000) // 0.1 uW units -> 1000 uW = 0 dBm
	put(qsfpTXBias+2*2, 3500)   // 2 uA units -> 7000 uA
	put(qsfpTXPower+2*2, 5000)  // -> 500 uW

	m := DecodeQSFP(lower, nil)
	if len(m.Lanes) != 4 {
		t.Fatalf("a QSFP has four lanes, got %d", len(m.Lanes))
	}
	l := m.Lanes[2]
	if l.Index != 3 {
		t.Errorf("lane index: want 3, got %d", l.Index)
	}
	if l.RXPowerUW != 1000 {
		t.Errorf("rx: want 1000 uW, got %d", l.RXPowerUW)
	}
	if l.TXBiasUA != 7000 {
		t.Errorf("bias: want 7000 uA, got %d", l.TXBiasUA)
	}
	if l.TXPowerUW != 500 {
		t.Errorf("tx: want 500 uW, got %d", l.TXPowerUW)
	}
	// And the lanes that were left alone must read as dark, not as noise.
	if m.Lanes[0].RXPowerUW != 0 {
		t.Errorf("lane 1 should be dark, got %d uW", m.Lanes[0].RXPowerUW)
	}
}

// An unimplemented optional field and a dead laser both read zero. Telling an
// operator a working module's transmitter is dark is worse than saying nothing.
func TestQSFPUnimplementedTXPowerIsNotADeadLaser(t *testing.T) {
	lower := make([]byte, QSFPLowerLen)
	lower[qsfpRXPower] = 0x27 // some light on lane 1, so the module is alive
	lower[qsfpRXPower+1] = 0x10
	m := DecodeQSFP(lower, nil)
	for _, l := range m.Lanes {
		if l.TXPowerOK {
			t.Errorf("lane %d: all-zero TX power reported as measured", l.Index)
		}
	}
	// One lane reporting makes it implemented for all of them.
	lower[qsfpTXPower+2] = 0x10
	for _, l := range DecodeQSFP(lower, nil).Lanes {
		if !l.TXPowerOK {
			t.Errorf("lane %d: implemented TX power reported as absent", l.Index)
		}
	}
}

// A module read before its first measurement cycle completes reports zeros
// everywhere, which is exactly what a dark link looks like.
func TestQSFPDataNotReadyIsDistinctFromDarkness(t *testing.T) {
	lower := make([]byte, QSFPLowerLen)
	if !QSFPDataReady(lower) {
		t.Error("a cleared status byte means data IS ready")
	}
	lower[qsfpStatus2] = 0x01
	if QSFPDataReady(lower) {
		t.Error("data-not-ready bit was ignored")
	}
}

func TestSFPDiagnosticsAreOptionalAndDetected(t *testing.T) {
	a0 := make([]byte, SFPA0Len)
	if SFPHasDiagnostics(a0) {
		t.Error("a module advertising no diagnostics was read as having them")
	}
	a0[sfpDiagType] = 0x40 // externally calibrated
	if !SFPHasDiagnostics(a0) {
		t.Error("externally-calibrated diagnostics not detected")
	}
	a0[sfpDiagType] = 0x20 // internally calibrated
	if !SFPHasDiagnostics(a0) {
		t.Error("internally-calibrated diagnostics not detected")
	}
}

func TestSFPDecodesOneLane(t *testing.T) {
	a0 := make([]byte, SFPA0Len)
	a2 := make([]byte, SFPA2Len)
	copy(a0[sfpVendor:], []byte("ACME            "))
	copy(a0[sfpPN:], []byte("SFP-10G-SR      "))
	a2[sfpTemp], a2[sfpTemp+1] = 0x19, 0x00       // 25 C
	a2[sfpRXPower], a2[sfpRXPower+1] = 0x27, 0x10 // 10000 -> 1000 uW
	m := DecodeSFP(a0, a2)
	if m.Vendor != "ACME" || m.PartNumber != "SFP-10G-SR" {
		t.Errorf("identity: vendor=%q pn=%q", m.Vendor, m.PartNumber)
	}
	if m.TempMilliC != 25000 {
		t.Errorf("temp: want 25000 mC, got %d", m.TempMilliC)
	}
	if len(m.Lanes) != 1 {
		t.Fatalf("an SFP has one lane, got %d", len(m.Lanes))
	}
	if m.Lanes[0].RXPowerUW != 1000 {
		t.Errorf("rx: want 1000 uW, got %d", m.Lanes[0].RXPowerUW)
	}
}

// Identity must survive a module with no diagnostic page at all.
func TestSFPWithoutDiagnosticsStillIdentifies(t *testing.T) {
	a0 := make([]byte, SFPA0Len)
	copy(a0[sfpVendor:], []byte("ACME            "))
	m := DecodeSFP(a0, nil)
	if m.Vendor != "ACME" {
		t.Errorf("vendor lost: %q", m.Vendor)
	}
	if m.TempOK || len(m.Lanes) != 0 {
		t.Error("absent diagnostics were reported as measured")
	}
}

func TestDBm(t *testing.T) {
	for _, tc := range []struct {
		uw   int
		want float64
	}{
		{1000, 0}, // 1 mW == 0 dBm
		{100, -10},
		{10, -20},
		{2000, 3.0103},
	} {
		got, ok := DBm(tc.uw)
		if !ok {
			t.Fatalf("%d uW reported as unmeasurable", tc.uw)
		}
		if math.Abs(got-tc.want) > 0.001 {
			t.Errorf("%d uW: want %.4f dBm, got %.4f", tc.uw, tc.want, got)
		}
	}
}

// log(0) is -Inf, and an operator reading "-inf dBm" reasonably takes it for a
// very weak signal rather than for no signal at all.
func TestNoLightIsNotAVerySmallNumber(t *testing.T) {
	if _, ok := DBm(0); ok {
		t.Error("zero microwatts reported as a measurable power")
	}
	if got := FormatDBm(0); got != "no signal" {
		t.Errorf("want %q, got %q", "no signal", got)
	}
	if got := FormatDBm(1000); got != "0.00 dBm" {
		t.Errorf("want %q, got %q", "0.00 dBm", got)
	}
}

// An unprogrammed vendor field is 0x00 or 0xff repeated. Rendering that into a
// terminal is how a port listing becomes unreadable.
func TestUnprintableVendorFieldsAreDropped(t *testing.T) {
	a0 := make([]byte, SFPA0Len)
	for i := 0; i < 16; i++ {
		a0[sfpVendor+i] = 0xff
	}
	if v := DecodeSFP(a0, nil).Vendor; v != "" {
		t.Errorf("unprogrammed vendor field rendered as %q", v)
	}
}

func TestKindOf(t *testing.T) {
	for id, want := range map[byte]Kind{
		0x03: SFP, 0x0c: QSFP, 0x0d: QSFP, 0x11: QSFP, 0x00: Unknown, 0xff: Unknown,
	} {
		if got := KindOf(id); got != want {
			t.Errorf("identifier %#02x: want %v, got %v", id, want, got)
		}
	}
}
