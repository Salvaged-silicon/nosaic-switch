package as4610

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// ONIE TlvInfo, which is what an ONIE whitebox keeps in its identity EEPROM.
//
// A defined format rather than this board's invention — ONIE specifies it and
// every switch that ships with ONIE carries it — so decoding it here means the
// switch can say what it is from its own EEPROM, the way the Arista boards
// read a prefdl.
//
// Confirmed on the hardware, 2026-09-16. This was written against an inference
// -- the part at i2c-9 0x50 is a 24c04 in the place ONIE's tooling expects, so
// TlvInfo was the reasonable guess -- and the spare unit's U-Boot prints
//
//	EEPROM: TlvInfo v1 len=160
//
// during boot, which settles both the format and the version this decodes.
//
// The header check below still fails cleanly and says what it found rather
// than returning a plausible model name made of the wrong bytes, because one
// unit having it proves nothing about a variant that does not.
const (
	onieHeaderID   = "TlvInfo\x00"
	onieHeaderLen  = 11 // 8 bytes of id, one version, two of length
	onieVersion    = 0x01
	onieTypeName   = 0x21 // product name
	onieTypePartNo = 0x22
	onieTypeSerial = 0x23
	onieTypeLabel  = 0x27 // label revision
	onieTypeDevVer = 0x26 // device version, one byte
	onieTypePlat   = 0x28 // platform name
)

func decodeONIE(raw []byte) (platformhal.Identity, error) {
	var id platformhal.Identity

	if len(raw) < onieHeaderLen {
		return id, fmt.Errorf("the board EEPROM returned %d bytes, too few for "+
			"a TlvInfo header", len(raw))
	}
	if !bytes.HasPrefix(raw, []byte(onieHeaderID)) {
		return id, fmt.Errorf("the board EEPROM does not begin with an ONIE "+
			"TlvInfo header (first bytes %#x); it holds something else, and "+
			"guessing at it would invent an identity", raw[:8])
	}
	if raw[8] != onieVersion {
		return id, fmt.Errorf("TlvInfo version %d, and this reads version %d",
			raw[8], onieVersion)
	}

	total := int(binary.BigEndian.Uint16(raw[9:11]))
	end := onieHeaderLen + total
	if end > len(raw) {
		// Truncated rather than refused: the header's own length is the
		// authority on how much there is, and a short read of a long EEPROM
		// still carries the fields at the front, which are the ones wanted.
		end = len(raw)
	}

	for p := onieHeaderLen; p+2 <= end; {
		typ := raw[p]
		n := int(raw[p+1])
		p += 2
		if p+n > end {
			break
		}
		val := strings.TrimRight(string(raw[p:p+n]), "\x00 ")
		switch typ {
		case onieTypeName:
			id.Model = val
		case onieTypeSerial:
			id.Serial = val
		case onieTypeLabel:
			id.Revision = val
		case onieTypeDevVer:
			if id.Revision == "" && n == 1 {
				id.Revision = fmt.Sprintf("%d", raw[p])
			}
		case onieTypePlat:
			id.SID = val
		case onieTypePartNo:
			if id.SID == "" {
				id.SID = val
			}
		}
		p += n
	}

	if id.Model == "" && id.Serial == "" {
		return id, fmt.Errorf("the board EEPROM has a TlvInfo header and no " +
			"product name or serial in it")
	}
	return id, nil
}
