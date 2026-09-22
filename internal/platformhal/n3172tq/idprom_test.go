package n3172tq

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// buildRecord lays out one record the way the measured dump does.
func buildRecord(magic bool, model, serial, part, rev string, mac []byte, macSize int) []byte {
	rec := make([]byte, idpromRecordLen)
	if magic {
		rec[0], rec[1] = 0xab, 0xab
	}
	put := func(off int, s string) { copy(rec[off:off+idpromFieldMax], s) }
	put(idpromOffVendor, "Cisco Systems, Inc.")
	put(idpromOffModel, model)
	put(idpromOffSerial, serial)
	put(idpromOffPart, part)
	put(idpromOffRev, rev)
	if mac != nil {
		copy(rec[idpromOffMAC:], mac)
		rec[idpromOffMACSize] = byte(macSize >> 8)
		rec[idpromOffMACSize+1] = byte(macSize)
	}
	return rec
}

// realPROM reproduces the lab chassis: board record then chassis record, with
// the values read off the hardware on 2026-09-18.
func realPROM() []byte {
	board := buildRecord(true, "N3K-C3172TQ-10GT", "FOC22010NYL", "73-15384-02", "R0", nil, 0)
	chassis := buildRecord(true, "N3K-C3172TQ-10GT", "FOC2201R1WZ", "68-4949-01", "S0",
		[]byte{0xb4, 0xde, 0x31, 0x3f, 0xa5, 0xc0}, 128)
	return append(board, chassis...)
}

func TestIDPROMChassisRecord(t *testing.T) {
	d, err := parseIDPROMRecord(realPROM(), idpromChassisRecord)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if d.Model != "N3K-C3172TQ-10GT" {
		t.Errorf("model = %q", d.Model)
	}
	if d.Serial != "FOC2201R1WZ" {
		t.Errorf("serial = %q, want the chassis serial from the asset label", d.Serial)
	}
	if d.Part != "68-4949-01" || d.Rev != "S0" {
		t.Errorf("part/rev = %q/%q", d.Part, d.Rev)
	}
	if d.MAC.String() != "b4:de:31:3f:a5:c0" {
		t.Errorf("mac = %q, want the address NX-OS uses", d.MAC)
	}
	if d.MACSize != 128 {
		t.Errorf("mac block = %d, want 128", d.MACSize)
	}
}

// The board record is a different serial and carries no MAC. Reporting the
// board serial where the chassis one belongs would be a plausible wrong
// answer, which is the failure this whole file is careful about.
func TestIDPROMBoardRecordIsDistinct(t *testing.T) {
	d, err := parseIDPROMRecord(realPROM(), idpromBoardRecord)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if d.Serial != "FOC22010NYL" {
		t.Errorf("board serial = %q", d.Serial)
	}
	if d.MAC != nil {
		t.Errorf("board record should carry no MAC, got %v", d.MAC)
	}
}

// A blank or foreign part must be refused, not decoded into confident noise.
func TestIDPROMRefusesWithoutMagic(t *testing.T) {
	bad := make([]byte, 2*idpromRecordLen)
	for i := range bad {
		bad[i] = 0xff
	}
	if _, err := parseIDPROMRecord(bad, idpromChassisRecord); err == nil {
		t.Fatal("a PROM with no magic was accepted")
	}
}

func TestIDPROMRefusesShortRead(t *testing.T) {
	if _, err := parseIDPROMRecord(realPROM()[:100], idpromBoardRecord); err == nil {
		t.Fatal("a truncated read was accepted")
	}
}

// Right magic, wrong offsets: every string field lands on binary. Reporting
// nothing is correct; reporting mojibake as a serial is not.
func TestIDPROMUnprintableFieldsAreDropped(t *testing.T) {
	rec := make([]byte, idpromRecordLen)
	rec[0], rec[1] = 0xab, 0xab
	for i := 2; i < idpromRecordLen; i++ {
		rec[i] = 0x01
	}
	d, err := parseIDPROMRecord(append(make([]byte, idpromRecordLen), rec...), idpromChassisRecord)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if d.Model != "" || d.Serial != "" || d.Rev != "" {
		t.Errorf("unprintable fields were reported: %+v", d)
	}
}

// Board() end to end over a fake sysfs, including the mux channel lookup.
func TestBoardReadsIdentity(t *testing.T) {
	root := t.TempDir()
	mux := filepath.Join(root, "bus/i2c/devices/0-0070")
	if err := os.MkdirAll(mux, 0o755); err != nil {
		t.Fatal(err)
	}
	bus := filepath.Join(root, "bus/i2c/devices/i2c-1")
	if err := os.MkdirAll(bus, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(bus, filepath.Join(mux, "channel-0")); err != nil {
		t.Fatal(err)
	}
	eep := filepath.Join(root, "bus/i2c/devices/1-0052")
	if err := os.MkdirAll(eep, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(eep, "eeprom"), realPROM(), 0o600); err != nil {
		t.Fatal(err)
	}

	ch := 0
	h := &hal{root: root, d: &Data{I2C: &KernelI2C{
		Mux:     &I2CDevice{Driver: "pca9548", Address: 0x70},
		Devices: []I2CDevice{{Driver: "24c512", Address: 0x52, Channel: &ch}},
	}}}

	id, err := h.Board()
	if err != nil {
		t.Fatalf("Board: %v", err)
	}
	if id.Model != "N3K-C3172TQ-10GT" || id.Serial != "FOC2201R1WZ" {
		t.Errorf("identity = %+v", id)
	}
	if id.Revision != "S0" || id.SID != "68-4949-01" {
		t.Errorf("identity = %+v", id)
	}
}

// No PROM instantiated is ErrUnsupported with a reason, not a crash.
func TestBoardWithoutPROM(t *testing.T) {
	h := &hal{root: t.TempDir(), d: &Data{}}
	_, err := h.Board()
	if err == nil {
		t.Fatal("expected an error with no i2c declared")
	}
	if !isUnsupported(err) {
		t.Errorf("want ErrUnsupported, got %v", err)
	}
}

func isUnsupported(err error) bool {
	for e := err; e != nil; {
		if e == platformhal.ErrUnsupported {
			return true
		}
		u, ok := e.(interface{ Unwrap() error })
		if !ok {
			return false
		}
		e = u.Unwrap()
	}
	return false
}
