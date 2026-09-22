package n3172tq

import (
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
)

// Reading the board's identity out of its ID PROM.
//
// The part is the 24c512 at 0x52 behind the mux, which the i2c service
// instantiates at boot and the at24 driver exposes as a plain `eeprom` file.
// Cisco's layout for it is not published, so everything here was measured on
// one chassis and is written down in platform/cisco-n3172tq/docs/todo.md
// beside the dump it came from.
//
// ⚠ THE OFFSETS ARE MEASURED, NOT SPECIFIED.
//
// They agree across the two records this PROM carries, which is the only
// corroboration there is -- two samples from one unit. Every field is
// therefore treated as optional: a record that does not start with the magic
// is refused outright, but a field that reads as empty or as unprintable
// bytes is dropped rather than reported, because a plausible-looking wrong
// serial is worse than no serial. Widen this only against another unit.
const (
	// Each record is a fixed-size block; the PROM holds two.
	idpromRecordLen = 4096
	// The board record comes first, the chassis record second. The chassis
	// one carries the serial on the asset label and the MAC.
	idpromBoardRecord   = 0
	idpromChassisRecord = 1

	// Field offsets within a record.
	idpromOffVendor  = 14  // "Cisco Systems, Inc."
	idpromOffModel   = 34  // "N3K-C3172TQ-10GT"
	idpromOffSerial  = 54  // "FOC2201R1WZ"
	idpromOffPart    = 74  // "68-4949-01"
	idpromOffRev     = 90  // "S0"
	idpromOffMAC     = 184 // six bytes, chassis record only
	idpromOffMACSize = 190 // two bytes big-endian: how many addresses

	// The longest a fixed field may run before the next one starts. Every
	// field seen is NUL-padded well inside this.
	idpromFieldMax = 20
)

// errNoIDPROM is what the caller turns into ErrUnsupported.
var errNoIDPROM = errors.New("no readable board ID PROM")

// idprom is one decoded record.
type idprom struct {
	Vendor  string
	Model   string
	Serial  string
	Part    string
	Rev     string
	MAC     net.HardwareAddr
	MACSize int
}

// idpromString reads one NUL-terminated fixed field.
//
// Anything unprintable means the offset is wrong for this PROM, and the
// honest answer to that is nothing rather than mojibake.
func idpromString(rec []byte, off int) string {
	if off+idpromFieldMax > len(rec) {
		return ""
	}
	b := rec[off : off+idpromFieldMax]
	if i := indexByte(b, 0); i >= 0 {
		b = b[:i]
	}
	s := strings.TrimSpace(string(b))
	for _, r := range s {
		if r < 0x20 || r > 0x7e {
			return ""
		}
	}
	return s
}

func indexByte(b []byte, c byte) int {
	for i := range b {
		if b[i] == c {
			return i
		}
	}
	return -1
}

// parseIDPROMRecord decodes record n out of a whole-PROM image.
func parseIDPROMRecord(image []byte, n int) (idprom, error) {
	start := n * idpromRecordLen
	if start+idpromRecordLen > len(image) {
		return idprom{}, fmt.Errorf("%w: record %d needs %d bytes, the PROM "+
			"read gave %d", errNoIDPROM, n, start+idpromRecordLen, len(image))
	}
	rec := image[start : start+idpromRecordLen]

	// The only structural check there is. Without it a blank or foreign part
	// decodes into confident nonsense.
	if rec[0] != 0xab || rec[1] != 0xab {
		return idprom{}, fmt.Errorf("%w: record %d does not start with the "+
			"0xabab magic (got %#02x %#02x)", errNoIDPROM, n, rec[0], rec[1])
	}

	d := idprom{
		Vendor: idpromString(rec, idpromOffVendor),
		Model:  idpromString(rec, idpromOffModel),
		Serial: idpromString(rec, idpromOffSerial),
		Part:   idpromString(rec, idpromOffPart),
		Rev:    idpromString(rec, idpromOffRev),
	}

	// The MAC is only on the chassis record; an all-zero field means this
	// record simply has none, which is not an error.
	mac := rec[idpromOffMAC : idpromOffMAC+6]
	zero := true
	for _, c := range mac {
		if c != 0 {
			zero = false
			break
		}
	}
	if !zero {
		d.MAC = net.HardwareAddr(append([]byte(nil), mac...))
		d.MACSize = int(rec[idpromOffMACSize])<<8 | int(rec[idpromOffMACSize+1])
	}
	return d, nil
}

// eepromPath finds the at24 the board declares, without hardcoding a bus.
//
// The kernel bus number depends on the order the mux registers its channels,
// so it is resolved through the same channel-N symlink the rest of this
// driver uses rather than assumed to be 1.
func (h *hal) eepromPath() (string, error) {
	if h.d == nil || h.d.I2C == nil {
		return "", fmt.Errorf("%w: this board declares no i2c devices", errNoIDPROM)
	}
	for _, dev := range h.d.I2C.Devices {
		if !strings.HasPrefix(dev.Driver, "24c") && !strings.HasPrefix(dev.Driver, "at24") {
			continue
		}
		bus := 0
		if dev.Channel != nil {
			b, err := h.muxChannelBus(*dev.Channel)
			if err != nil {
				return "", err
			}
			bus = b
		}
		p := filepath.Join(h.root, "bus/i2c/devices",
			fmt.Sprintf("%d-%04x", bus, dev.Address), "eeprom")
		if _, err := os.Stat(p); err != nil {
			return "", fmt.Errorf("%w: %s is declared but not present -- are "+
				"this board's i2c devices instantiated?", errNoIDPROM, p)
		}
		return p, nil
	}
	return "", fmt.Errorf("%w: this board declares no at24 ID PROM", errNoIDPROM)
}
