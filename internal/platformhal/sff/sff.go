// Package sff decodes pluggable transceiver diagnostics.
//
// This is the board-independent half of reading an optic. A module's
// diagnostic bytes are defined by SFF-8472 (SFP/SFP+) and SFF-8636 (QSFP+),
// which are module standards: the same optic reports the same bytes in the
// same places whichever switch it is plugged into. Only *fetching* those bytes
// is board business -- an SMBus behind an FPGA on one board, a CPLD on the
// next -- and that is what the platform HAL provides.
//
// Keeping the split here is the point. The alternative, which every vendor
// tree arrives at eventually, is a per-board transceiver driver that decodes
// as well as reads, so the arithmetic is written once per board and wrong in a
// different way each time.
//
// Nothing here does I/O. It takes bytes and returns numbers, so it is testable
// against captured module dumps with no switch present.
package sff

import (
	"fmt"
	"math"
	"strings"
)

// Kind is which standard a module's diagnostics follow.
type Kind int

const (
	Unknown Kind = iota
	SFP          // SFF-8472: one lane, diagnostics at i2c 0x51
	QSFP         // SFF-8636: four lanes, diagnostics in page 00h at 0x50
)

func (k Kind) String() string {
	switch k {
	case SFP:
		return "SFP"
	case QSFP:
		return "QSFP"
	}
	return "unknown"
}

// Identifier is byte 0 of the base page, SFF-8024 table 4-1. Only the values
// that decide which decode applies are named; the rest are reported as their
// number rather than guessed at.
func KindOf(identifier byte) Kind {
	switch identifier {
	case 0x03: // SFP/SFP+/SFP28
		return SFP
	case 0x0c, 0x0d, 0x11, 0x18, 0x19: // QSFP, QSFP+, QSFP28, QSFP-DD, OSFP
		return QSFP
	}
	return Unknown
}

// Lane is one optical lane's measurements. A QSFP has four, an SFP one.
//
// Powers are microwatts as the module reports them, because that is the unit
// the standard defines and converting on the way in loses the distinction
// between "zero" and "too low to measure". Use DBm for display.
type Lane struct {
	Index     int
	TXBiasUA  int // microamps
	TXPowerUW int // microwatts
	RXPowerUW int // microwatts
	// TXPowerOK is false when the module does not implement transmit power
	// measurement, which is optional in SFF-8636. Reporting an unimplemented
	// field as 0 uW reads as a dead laser.
	TXPowerOK bool
}

// Module is everything one transceiver reports about itself.
type Module struct {
	Kind         Kind
	Identifier   byte
	Vendor       string
	PartNumber   string
	SerialNumber string
	TempMilliC   int // millidegrees C
	VccMV        int // millivolts
	Lanes        []Lane
	// TempOK and VccOK are false when the module reports no diagnostics at
	// all, which is legal: SFF-8472 makes the whole diagnostic page optional.
	TempOK bool
	VccOK  bool
}

// DiagnosticsAllZero reports whether the whole diagnostic block reads as zero.
//
// A module may implement diagnostics and populate nothing. SFF-8636 byte 220
// on this board's CISCO-AVAGO QSFP+ modules advertises receive power
// monitoring, and every diagnostic byte then reads 0x00 -- on cages carrying a
// healthy OSPF adjacency. A raw SMBus read of those same bytes returns the
// same zeroes, so the module is the source and not this decode.
//
// It matters because zeroes render as "0.0 C" and "no signal" on every lane,
// which is indistinguishable from a dead link. That cost real time during the
// Ethernet49 investigation on 2026-09-19, where the display was read as
// evidence of no light when it was evidence of nothing at all. Temperature is
// the giveaway: a powered module always has one.
func (m Module) DiagnosticsAllZero() bool {
	if m.TempMilliC != 0 || m.VccMV != 0 {
		return false
	}
	for _, l := range m.Lanes {
		if l.TXBiasUA != 0 || l.RXPowerUW != 0 || l.TXPowerUW != 0 {
			return false
		}
	}
	return len(m.Lanes) > 0
}

// DBm converts a power in microwatts to dBm.
//
// Zero is not 0 dBm, it is no light, and log(0) is -Inf. A module reporting
// zero means the reading is below what it can measure, so that is reported as
// a distinct answer rather than as a number -- a displayed "-inf dBm" invites
// somebody to read it as a very small signal rather than as no signal.
func DBm(microwatts int) (dbm float64, measurable bool) {
	if microwatts <= 0 {
		return 0, false
	}
	return 10 * math.Log10(float64(microwatts)/1000.0), true
}

// FormatDBm renders a power for an operator, including the no-light case.
func FormatDBm(microwatts int) string {
	if d, ok := DBm(microwatts); ok {
		return fmt.Sprintf("%.2f dBm", d)
	}
	return "no signal"
}

func be16(b []byte, off int) int {
	if off+1 >= len(b) {
		return 0
	}
	return int(b[off])<<8 | int(b[off+1])
}

func sbe16(b []byte, off int) int {
	v := be16(b, off)
	if v >= 0x8000 {
		v -= 0x10000
	}
	return v
}

// text reads a fixed-width space-padded ASCII field, as both standards use for
// vendor strings. Bytes outside printable ASCII are dropped rather than
// rendered: an unprogrammed field is 0x00 or 0xff repeated, and emitting that
// into a terminal is how a listing becomes unreadable.
func text(b []byte, off, n int) string {
	if off+n > len(b) {
		return ""
	}
	var sb strings.Builder
	for _, c := range b[off : off+n] {
		if c >= 0x20 && c < 0x7f {
			sb.WriteByte(c)
		}
	}
	return strings.TrimSpace(sb.String())
}
