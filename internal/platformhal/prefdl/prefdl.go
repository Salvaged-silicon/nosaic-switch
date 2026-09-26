/* SPDX-License-Identifier: Apache-2.0 */

// Package prefdl decodes Arista's board identity structure.
//
// A prefdl is what an Arista switch knows about itself: model, serial,
// hardware revision, base MAC, and on some boards electrical settings the
// board was calibrated with. It lives on an i2c SEEPROM, so a running switch
// can be identified from itself rather than from what it was told at build
// time -- which is the whole reason to read it. An image that carries a MAC
// in a configuration file is an image for one chassis.
//
// THE FORMAT IS ASCII, NOT THE BINARY ONE. Arista's own open driver
// (aristanetworks/sonic, arista/core/prefdl.py) decodes a binary TLV, and the
// field codes here are the same -- but on this board every field is text: a
// two-hex-digit code, a four-hex-digit length, then that many characters. The
// codes were taken from that driver; the framing was derived from a real
// device, because no document describes it.
//
// Layout, established from a DCS-7150S-52-CL and matching the field lengths
// the vendor's driver declares for the fixed fields:
//
//	[0:4]    version, "0002"
//	[4:16]   PCA, 12 characters
//	[16:27]  serial number, 11 characters
//	[27:30]  KVN, 3 characters
//	[30:..]  TLVs, ended by code 00 with length 0
//	[tail]   CRC32, 8 hex characters
//
// ⚠ THE CRC IS NOT CHECKED. It is carried through as text so a caller can see
// it, and verifying it would mean guessing which bytes it covers and with
// which of zlib's conventions -- a check that is wrong in an unknown direction
// is worse than no check, because it rejects good hardware. Whether the
// structure decoded is judged on the framing instead: the TLV chain has to
// reach its terminator exactly.
package prefdl

import (
	"fmt"
	"strconv"
	"strings"
)

// Field codes, from aristanetworks/sonic arista/core/prefdl.py.
const (
	CodeEnd      = 0x00
	CodeMfgTime  = 0x02
	CodeSKU      = 0x03
	CodeASY      = 0x04
	CodeMAC      = 0x05
	CodeHwAPI    = 0x0a
	CodeHwRev    = 0x0b
	CodeSID      = 0x0c
	CodePCA      = 0x0d
	CodeSerial   = 0x0e
	CodeKVN      = 0x0f
	CodeMfgTime2 = 0x17
)

// Prefdl is a decoded board identity.
//
// Fields keeps every TLV, by code, including ones this package has no name
// for -- a board that carries something unexpected should not have it silently
// dropped, and one of them turned out to matter. See VendorData.
type Prefdl struct {
	Version string
	PCA     string
	Serial  string
	KVN     string

	SKU     string // the marketing model, e.g. "DCS-7150S-52-CL"
	SID     string // the vendor's internal board name, e.g. "SantaRosa"
	MAC     string // 12 hex digits, unseparated
	HwRev   string
	HwAPI   string
	MfgTime string
	ASY     string

	// VendorData is TLV 0x09, which is not in the vendor's own field table
	// and is board-specific. On a 7150S-52 it carries the Alta core rail
	// voltages -- {'AltaVdd':1.01,'AltaVdds':1.0} -- which is calibration
	// data for that chassis and not a constant of the design.
	VendorData string

	Fields map[int]string
	CRC    string
}

// MACAddress returns the base MAC in the usual colon-separated form, or "" if
// the prefdl did not carry one that looks like a MAC.
func (p *Prefdl) MACAddress() string {
	if len(p.MAC) != 12 {
		return ""
	}
	var b strings.Builder
	for i := 0; i < 12; i += 2 {
		if i > 0 {
			b.WriteByte(':')
		}
		b.WriteString(strings.ToLower(p.MAC[i : i+2]))
	}
	return b.String()
}

const (
	verLen    = 4
	pcaLen    = 12
	serialLen = 11
	kvnLen    = 3
	headerLen = verLen + pcaLen + serialLen + kvnLen
	crcLen    = 8
)

// Parse decodes a prefdl image.
//
// `raw` may be longer than the structure -- a SEEPROM read usually is, and the
// tail is whatever the erased part of the device reads as. Parsing stops at
// the terminator and the remainder is ignored.
func Parse(raw []byte) (*Prefdl, error) {
	// Trim at the first byte that cannot be part of the text, which is how
	// the end of a short structure in a larger device presents.
	s := string(raw)
	if i := strings.IndexFunc(s, func(r rune) bool { return r < 0x20 || r > 0x7e }); i >= 0 {
		s = s[:i]
	}
	if len(s) < headerLen {
		return nil, fmt.Errorf("prefdl: %d readable characters, need at least %d "+
			"for the fixed header -- this is probably not a prefdl", len(s), headerLen)
	}

	p := &Prefdl{
		Version: s[0:verLen],
		PCA:     s[verLen : verLen+pcaLen],
		Serial:  s[verLen+pcaLen : verLen+pcaLen+serialLen],
		KVN:     s[verLen+pcaLen+serialLen : headerLen],
		Fields:  map[int]string{},
	}

	i := headerLen
	terminated := false
	for i+6 <= len(s) {
		code, err := strconv.ParseUint(s[i:i+2], 16, 8)
		if err != nil {
			return nil, fmt.Errorf("prefdl: field code %q at offset %d is not hex",
				s[i:i+2], i)
		}
		n, err := strconv.ParseUint(s[i+2:i+6], 16, 16)
		if err != nil {
			return nil, fmt.Errorf("prefdl: length %q at offset %d is not hex",
				s[i+2:i+6], i+2)
		}
		if code == CodeEnd {
			i += 6
			terminated = true
			break
		}
		if i+6+int(n) > len(s) {
			return nil, fmt.Errorf("prefdl: field %#02x at offset %d claims %d "+
				"characters and only %d remain", code, i, n, len(s)-(i+6))
		}
		p.Fields[int(code)] = s[i+6 : i+6+int(n)]
		i += 6 + int(n)
	}
	if !terminated {
		// The framing IS the integrity check here, since the CRC is not
		// verified -- so running off the end is a decode failure and not
		// something to return a half-filled struct for.
		return nil, fmt.Errorf("prefdl: ran out of data at offset %d without "+
			"reaching the end marker; the framing does not hold", i)
	}
	if len(s) >= i+crcLen {
		p.CRC = s[i : i+crcLen]
	}

	p.SKU = p.Fields[CodeSKU]
	p.SID = p.Fields[CodeSID]
	p.MAC = p.Fields[CodeMAC]
	p.HwRev = p.Fields[CodeHwRev]
	p.HwAPI = p.Fields[CodeHwAPI]
	p.MfgTime = p.Fields[CodeMfgTime]
	p.ASY = p.Fields[CodeASY]
	p.VendorData = p.Fields[0x09]

	// A fixed-field serial can also appear as a TLV; prefer the TLV, which is
	// the one the vendor's own reader treats as authoritative.
	if v, ok := p.Fields[CodeSerial]; ok && v != "" {
		p.Serial = v
	}
	if v, ok := p.Fields[CodePCA]; ok && v != "" {
		p.PCA = v
	}
	return p, nil
}
