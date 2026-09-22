package as4610

import (
	"context"
	"errors"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// fakeBus is a board that answers from a table, so the decode above the bus
// can be tested without one. Writes are recorded rather than applied, because
// what matters about a fan command is the register value it produces.
type fakeBus struct {
	regs    map[int]map[int]byte // addr -> reg -> value
	mem     map[int][]byte       // addr -> a device with an internal offset
	writes  []write
	failAll bool
}

type write struct {
	addr, reg int
	val       byte
}

func (f *fakeBus) ReadReg(addr, reg int) (byte, error) {
	if f.failAll {
		return 0, errors.New("no device")
	}
	d, ok := f.regs[addr]
	if !ok {
		return 0, errors.New("no device")
	}
	v, ok := d[reg]
	if !ok {
		return 0, errors.New("no such register")
	}
	return v, nil
}

func (f *fakeBus) WriteReg(addr, reg int, v byte) error {
	if f.failAll {
		return errors.New("no device")
	}
	f.writes = append(f.writes, write{addr, reg, v})
	return nil
}

func (f *fakeBus) ReadWord(addr, reg int) (uint16, error) {
	hi, err := f.ReadReg(addr, reg)
	if err != nil {
		return 0, err
	}
	lo, err := f.ReadReg(addr, reg+1)
	if err != nil {
		return 0, err
	}
	return uint16(hi)<<8 | uint16(lo), nil
}

func (f *fakeBus) ReadAt(addr, offset, n int) ([]byte, error) {
	if f.failAll {
		return nil, errors.New("no device")
	}
	if m, ok := f.mem[addr]; ok {
		out := make([]byte, n)
		copy(out, m[min(offset, len(m)):])
		return out, nil
	}
	out := make([]byte, n)
	for i := 0; i < n; i++ {
		v, err := f.ReadReg(addr, offset+i)
		if err != nil {
			return nil, err
		}
		out[i] = v
	}
	return out, nil
}

func (f *fakeBus) Close() error { return nil }

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// theBoard is the AS4610-54T's own map, so the tests exercise what ships
// rather than a shape invented for them.
func theBoard() platformhal.Config {
	return platformhal.Config{I2C: &platformhal.I2CMap{
		Controller: platformhal.I2CAddr{Bus: 0, Addr: 0x30},
		Sensors: []platformhal.I2CSensor{
			{Name: "board", Part: "lm77", I2CAddr: platformhal.I2CAddr{Bus: 9, Addr: 0x48}},
		},
		Cages:  &platformhal.I2CCages{FirstBus: 2, Count: 6},
		EEPROM: &platformhal.I2CAddr{Bus: 9, Addr: 0x50},
	}}
}

func openWith(t *testing.T, f *fakeBus) *HAL {
	t.Helper()
	old := openBus
	openBus = func(int) (bus, error) { return f, nil }
	t.Cleanup(func() { openBus = old })

	h, err := Open(theBoard())
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	return h.(*HAL)
}

// A CPLD that is not there must be an error at Open, not a wrong number later.
// The failure mode this guards against is specific: every access refused,
// which reads as a broken bus rather than as a board whose controller is
// somewhere else.
func TestOpenRefusesAnAbsentController(t *testing.T) {
	old := openBus
	openBus = func(int) (bus, error) { return &fakeBus{failAll: true}, nil }
	defer func() { openBus = old }()

	if _, err := Open(theBoard()); err == nil {
		t.Fatal("Open succeeded with no controller on the bus")
	}
}

func TestOpenRefusesABoardWithNoMap(t *testing.T) {
	_, err := Open(platformhal.Config{})
	if !errors.Is(err, platformhal.ErrUnsupported) {
		t.Fatalf("want ErrUnsupported for a board with no i2c map, got %v", err)
	}
}

// The LM77 reading is signed, and the sign is the part worth a test: an
// arithmetic shift on a value already widened would turn -1 °C into 4095.5 °C,
// and a thermal loop believes four thousand degrees before a person does.
func TestLM77Decode(t *testing.T) {
	for _, c := range []struct {
		name string
		raw  uint16
		want int
	}{
		{"zero", 0x0000, 0},
		{"12.5C", 0x00c8, 12500},
		{"25C", 0x0190, 25000},
		{"49C, as read on the lab unit", 0x0310, 49000},
		{"half a degree", 0x0008, 500},
		{"minus one", 0xfff0, -1000},
		{"minus 25", 0xfe70, -25000},
		// Truncation toward zero, as the kernel driver does it. An
		// arithmetic shift would give -500 here.
		{"just below zero, where shift and divide disagree", 0xfffc, 0},
	} {
		if got := lm77MilliC(c.raw); got != c.want {
			t.Errorf("%s: lm77MilliC(%#04x) = %d, want %d", c.name, c.raw, got, c.want)
		}
	}
}

func TestTemperatures(t *testing.T) {
	f := &fakeBus{regs: map[int]map[int]byte{
		0x30: {0x11: 0x0d},
		0x48: {0x00: 0x03, 0x01: 0x10}, // 0x0310 = 49 C, as the lab unit reads
	}}
	h := openWith(t, f)

	temps, err := h.Temperatures()
	if err != nil {
		t.Fatalf("Temperatures: %v", err)
	}
	if temps["board"] != 49000 {
		t.Errorf("board = %d millidegrees, want 49000", temps["board"])
	}
}

// The fan duty round-trip. Truncating instead of rounding makes every
// requested duty come out one step low, which a thermal loop compensates for
// by asking for more, indefinitely.
func TestFanDutyRoundTrip(t *testing.T) {
	for _, c := range []struct {
		pct int
		reg byte
	}{
		{0, 0}, {12, 1}, {25, 2}, {50, 4}, {75, 6}, {100, 8},
	} {
		if got := fanDutyToReg(c.pct); got != c.reg {
			t.Errorf("fanDutyToReg(%d) = %d, want %d", c.pct, got, c.reg)
		}
	}
	// And back, within one step.
	for reg := byte(0); reg <= 8; reg++ {
		pct := fanRegToDuty(reg)
		if back := fanDutyToReg(pct); back != reg {
			t.Errorf("duty %d%% (reg %d) came back as reg %d", pct, reg, back)
		}
	}
}

// A register above 8 decodes past 100%, which is not a reading. It is clamped
// rather than reported, and the raw value is what says something is wrong.
func TestFanDutyClampsImplausibleRegister(t *testing.T) {
	if got := fanRegToDuty(0x0f); got != 100 {
		t.Errorf("fanRegToDuty(0x0f) = %d, want it clamped to 100", got)
	}
}

// A fan controller that can be told to stop is one that will eventually be
// told to stop by a bug.
func TestSetFanPercentHoldsTheFloor(t *testing.T) {
	f := &fakeBus{regs: map[int]map[int]byte{0x30: {0x11: 0x0d}}}
	h := openWith(t, f)

	if _, err := h.SetFanPercent(0); err != nil {
		t.Fatalf("SetFanPercent: %v", err)
	}
	if len(f.writes) != 1 {
		t.Fatalf("want one write, got %d", len(f.writes))
	}
	w := f.writes[0]
	if w.addr != 0x30 || w.reg != regFanPWM {
		t.Errorf("wrote %#02x register %#02x, want the fan duty on the CPLD", w.addr, w.reg)
	}
	if got := fanRegToDuty(w.val); got < h.FanFloorPercent() {
		t.Errorf("commanded %d%%, below this board's floor of %d%%",
			got, h.FanFloorPercent())
	}
}

// Both bits, and they answer different questions: a supply that is fitted with
// nothing plugged into it is the normal state of a second PSU, not a fault.
func TestPSUDecode(t *testing.T) {
	// 0x0d as read on the lab unit: psu1 present and not ok, psu2 both.
	f := &fakeBus{regs: map[int]map[int]byte{0x30: {0x11: 0x0d}}}
	h := openWith(t, f)

	psus, err := h.PSUs()
	if err != nil {
		t.Fatalf("PSUs: %v", err)
	}
	if len(psus) != 2 {
		t.Fatalf("got %d supplies, want 2", len(psus))
	}
	if !psus[0].Present || psus[0].OK {
		t.Errorf("psu1 = %+v, want present and not ok", psus[0])
	}
	if !psus[1].Present || !psus[1].OK {
		t.Errorf("psu2 = %+v, want present and ok", psus[1])
	}
}

// Releasing the front panel is five writes and all five matter: any one left
// asserted gives the same symptom, every port down with no error anywhere.
func TestReleaseSwitchChipWritesEveryPHYReset(t *testing.T) {
	f := &fakeBus{regs: map[int]map[int]byte{0x30: {0x11: 0x0d}}}
	h := openWith(t, f)

	if err := h.ReleaseSwitchChip(context.Background()); err != nil {
		t.Fatalf("ReleaseSwitchChip: %v", err)
	}
	if len(f.writes) != len(phyRelease) {
		t.Fatalf("%d writes, want %d", len(f.writes), len(phyRelease))
	}
	for i, w := range f.writes {
		if w.reg != phyRelease[i].reg || w.val != phyRelease[i].val {
			t.Errorf("write %d: %#02x=%#02x, want %#02x=%#02x",
				i, w.reg, w.val, phyRelease[i].reg, phyRelease[i].val)
		}
	}
}

// Selecting an SFF page is refused rather than done unsafely, and the refusal
// has to be ErrUnsupported so that ReadModule's best-effort path takes it as
// "this board cannot" rather than as a bus failure.
func TestReadModuleBytesRefusesAPageSelect(t *testing.T) {
	f := &fakeBus{regs: map[int]map[int]byte{0x30: {0x11: 0x0d}}}
	h := openWith(t, f)

	if _, err := h.ReadModuleBytes(1, 0x50, 0, 128, 128); !errors.Is(err, platformhal.ErrUnsupported) {
		t.Fatalf("want ErrUnsupported for a page select, got %v", err)
	}
}

func TestReadModuleBytesChecksTheCage(t *testing.T) {
	f := &fakeBus{regs: map[int]map[int]byte{0x30: {0x11: 0x0d}}}
	h := openWith(t, f)

	if h.CageCount() != 6 {
		t.Errorf("CageCount = %d, want 6", h.CageCount())
	}
	for _, cage := range []int{0, 7, -1} {
		if _, err := h.ReadModuleBytes(cage, 0x50, -1, 0, 1); err == nil {
			t.Errorf("cage %d was accepted; this board has 6", cage)
		}
	}
}

// The board's identity comes out of its own EEPROM, so a running switch can be
// identified from itself rather than from what it was built as.
func TestBoardIdentityFromONIETLV(t *testing.T) {
	eeprom := buildTLV(map[byte]string{
		onieTypeName:   "AS4610-54T",
		onieTypeSerial: "AS4610T2024001",
		onieTypeLabel:  "R0C",
		onieTypePlat:   "arm-accton-as4610-54-r0",
	})
	f := &fakeBus{
		regs: map[int]map[int]byte{0x30: {0x11: 0x0d}},
		mem:  map[int][]byte{0x50: eeprom},
	}
	h := openWith(t, f)

	id, err := h.Board()
	if err != nil {
		t.Fatalf("Board: %v", err)
	}
	if id.Model != "AS4610-54T" || id.Serial != "AS4610T2024001" ||
		id.Revision != "R0C" || id.SID != "arm-accton-as4610-54-r0" {
		t.Errorf("identity = %+v", id)
	}
}

// An EEPROM holding something else must say so rather than invent a model name
// out of whatever bytes are there. It has never been read on this board, so
// this is the case most likely to actually happen.
func TestBoardIdentityRefusesNonTLV(t *testing.T) {
	f := &fakeBus{
		regs: map[int]map[int]byte{0x30: {0x11: 0x0d}},
		mem:  map[int][]byte{0x50: make([]byte, 256)},
	}
	h := openWith(t, f)

	if _, err := h.Board(); err == nil {
		t.Fatal("an EEPROM of zeroes was accepted as an identity")
	}
}

func buildTLV(fields map[byte]string) []byte {
	var body []byte
	// Sorted by type so the encoding is stable, which matters only for the
	// test's own readability.
	for _, typ := range []byte{onieTypeName, onieTypePartNo, onieTypeSerial,
		onieTypeDevVer, onieTypeLabel, onieTypePlat} {
		v, ok := fields[typ]
		if !ok {
			continue
		}
		body = append(body, typ, byte(len(v)))
		body = append(body, v...)
	}
	out := make([]byte, 0, onieHeaderLen+len(body)+8)
	out = append(out, onieHeaderID...)
	out = append(out, onieVersion)
	out = append(out, byte(len(body)>>8), byte(len(body)))
	out = append(out, body...)
	// Pad to a plausible read length.
	for len(out) < 256 {
		out = append(out, 0)
	}
	return out
}
