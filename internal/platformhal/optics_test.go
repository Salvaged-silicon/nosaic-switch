package platformhal

import (
	"errors"
	"strings"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal/sff"
)

// fakeOptics is a board whose cages hold whatever the test puts in them. It
// records every read so the tests can assert on which addresses and pages were
// touched -- reading the wrong i2c address is the failure this seam exists to
// prevent, and it produces numbers rather than an error.
type fakeOptics struct {
	cages  map[int]map[int][]byte // cage -> i2c addr -> bytes
	reads  []read
	fail   error
	noPage bool
}

type read struct{ cage, addr, page, offset, n int }

func (f *fakeOptics) CageCount() int { return 54 }

func (f *fakeOptics) ReadModuleBytes(cage, addr, page, offset, n int) ([]byte, error) {
	f.reads = append(f.reads, read{cage, addr, page, offset, n})
	if f.fail != nil {
		return nil, f.fail
	}
	if f.noPage && page >= 0 {
		return nil, errors.New("this board cannot select a page")
	}
	b := f.cages[cage][addr]
	if offset >= len(b) {
		return make([]byte, n), nil
	}
	out := make([]byte, n)
	copy(out, b[offset:])
	return out, nil
}

func qsfpCage(temp uint16, rx1 uint16) []byte {
	b := make([]byte, 256)
	b[0] = 0x0d // QSFP+
	b[22], b[23] = byte(temp>>8), byte(temp)
	b[34], b[35] = byte(rx1>>8), byte(rx1)
	return b
}

func TestAQSFPIsReadInPageZeroAtFifty(t *testing.T) {
	f := &fakeOptics{cages: map[int]map[int][]byte{
		52: {0x50: qsfpCage(0x1900, 10000)},
	}}
	m, err := ReadModule(f, 52, false)
	if err != nil {
		t.Fatalf("ReadModule: %v", err)
	}
	if m.Kind != sff.QSFP {
		t.Errorf("kind: want QSFP, got %v", m.Kind)
	}
	if m.TempMilliC != 25000 {
		t.Errorf("temp: want 25000, got %d", m.TempMilliC)
	}
	if m.Lanes[0].RXPowerUW != 1000 {
		t.Errorf("rx lane 1: want 1000 uW, got %d", m.Lanes[0].RXPowerUW)
	}
	// A QSFP's diagnostics must never be sought at 0x51: that address is an
	// SFP's diagnostic page and on a QSFP it is not defined.
	for _, r := range f.reads {
		if r.addr == 0x51 {
			t.Errorf("read 0x51 on a QSFP: %+v", r)
		}
	}
}

// The two standards disagree about where diagnostics live, so the type has to
// be established before the read rather than assumed from the cage.
func TestAnSFPIsReadAtFiftyOneAndOnlyWhenItSaysSo(t *testing.T) {
	a0 := make([]byte, 96)
	a0[0] = 0x03 // SFP
	a0[92] = 0x40
	copy(a0[20:], []byte("ACME            "))
	a2 := make([]byte, 112)
	a2[96], a2[97] = 0x19, 0x00

	f := &fakeOptics{cages: map[int]map[int][]byte{1: {0x50: a0, 0x51: a2}}}
	m, err := ReadModule(f, 1, true)
	if err != nil {
		t.Fatalf("ReadModule: %v", err)
	}
	if m.Kind != sff.SFP || m.TempMilliC != 25000 || m.Vendor != "ACME" {
		t.Errorf("decoded wrong: %+v", m)
	}
	saw51 := false
	for _, r := range f.reads {
		if r.addr == 0x51 {
			saw51 = true
		}
	}
	if !saw51 {
		t.Error("a module advertising diagnostics was never read at 0x51")
	}
}

func TestAnSFPWithoutDiagnosticsIsNotProbedAtFiftyOne(t *testing.T) {
	a0 := make([]byte, 96)
	a0[0] = 0x03
	a0[92] = 0x00 // no diagnostics advertised
	f := &fakeOptics{cages: map[int]map[int][]byte{1: {0x50: a0}}}
	if _, err := ReadModule(f, 1, true); err != nil {
		t.Fatalf("ReadModule: %v", err)
	}
	for _, r := range f.reads {
		if r.addr == 0x51 {
			t.Error("probed 0x51 on a module that advertises no diagnostic page; " +
				"a floating bus decodes to plausible nonsense")
		}
	}
}

// Light levels must survive a board that cannot write a page select. They are
// in the always-mapped lower half; only the vendor strings need the page.
func TestLightLevelsSurviveABoardThatCannotSelectPages(t *testing.T) {
	f := &fakeOptics{noPage: true, cages: map[int]map[int][]byte{
		52: {0x50: qsfpCage(0x1900, 10000)},
	}}
	m, err := ReadModule(f, 52, true)
	if err != nil {
		t.Fatalf("a page-select failure lost the whole read: %v", err)
	}
	if m.Lanes[0].RXPowerUW != 1000 {
		t.Errorf("rx lost: %d", m.Lanes[0].RXPowerUW)
	}
	if m.Vendor != "" {
		t.Errorf("vendor invented without a page select: %q", m.Vendor)
	}
}

func TestAnEmptyCageIsAnErrorNotZeroes(t *testing.T) {
	// 0x00 is not a valid SFF-8024 identifier; an absent module reads as 0x00
	// or 0xff, and decoding either as a transceiver reports a dark one.
	f := &fakeOptics{cages: map[int]map[int][]byte{7: {0x50: make([]byte, 256)}}}
	if _, err := ReadModule(f, 7, false); err == nil {
		t.Error("an empty cage decoded as a module")
	}
}

func TestCageNumberIsRangeChecked(t *testing.T) {
	f := &fakeOptics{cages: map[int]map[int][]byte{}}
	for _, c := range []int{0, -1, 55} {
		if _, err := ReadModule(f, c, false); err == nil {
			t.Errorf("cage %d accepted", c)
		} else if !strings.Contains(err.Error(), "this board has 54") {
			t.Errorf("cage %d: unhelpful error %v", c, err)
		}
	}
}

func TestABusErrorIsReported(t *testing.T) {
	f := &fakeOptics{fail: errors.New("smbus timeout"), cages: map[int]map[int][]byte{}}
	_, err := ReadModule(f, 1, false)
	if err == nil || !strings.Contains(err.Error(), "smbus timeout") {
		t.Errorf("bus error swallowed: %v", err)
	}
}
