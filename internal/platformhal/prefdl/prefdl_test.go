/* SPDX-License-Identifier: Apache-2.0 */
package prefdl

import "testing"

// The real thing, read byte for byte off this lab's DCS-7150S-52-CL at host
// i2c-1 address 0x52 on 2026-09-26. A synthetic vector would only prove the
// parser agrees with my idea of the format.
const realDevice = "0002PCA0007922A0JPE1706068011104000cASY0058122A0" +
	"09001f{'AltaVdd':1.01,'AltaVdds':1.0}" +
	"0A000505.0111000501.000B000512.04" +
	"05000c444ca8315daa" +
	"02000e20170217015036" +
	"0C0009SantaRosa" +
	"03000fDCS-7150S-52-CL" +
	"000000E463F1A1"

func TestDecodesThisLabsBoard(t *testing.T) {
	p, err := Parse([]byte(realDevice))
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	for _, c := range []struct{ name, got, want string }{
		{"SKU", p.SKU, "DCS-7150S-52-CL"},
		{"SID", p.SID, "SantaRosa"},
		{"MAC", p.MAC, "444ca8315daa"},
		{"HwRev", p.HwRev, "12.04"},
		{"HwAPI", p.HwAPI, "05.01"},
		{"MfgTime", p.MfgTime, "20170217015036"},
		{"serial", p.Serial, "JPE17060680"},
		{"CRC", p.CRC, "E463F1A1"},
		{"vendor data", p.VendorData, "{'AltaVdd':1.01,'AltaVdds':1.0}"},
	} {
		if c.got != c.want {
			t.Errorf("%s = %q, want %q", c.name, c.got, c.want)
		}
	}
}

// The reason this package exists: config/network.conf states this MAC by hand,
// which makes the image chassis-specific.
func TestMACComesOutInTheFormANetworkConfigWants(t *testing.T) {
	p, err := Parse([]byte(realDevice))
	if err != nil {
		t.Fatal(err)
	}
	if got, want := p.MACAddress(), "44:4c:a8:31:5d:aa"; got != want {
		t.Errorf("MACAddress() = %q, want %q", got, want)
	}
}

// A SEEPROM read returns the whole device, not the structure, and the tail is
// whatever the unwritten part reads as.
func TestTrailingDeviceContentsAreIgnored(t *testing.T) {
	padded := append([]byte(realDevice), make([]byte, 96)...)
	p, err := Parse(padded)
	if err != nil {
		t.Fatalf("Parse with trailing zeros: %v", err)
	}
	if p.SKU != "DCS-7150S-52-CL" {
		t.Errorf("SKU = %q with padding, want it unaffected", p.SKU)
	}
}

// Framing is the only integrity check, because the CRC is not verified -- so
// it has to actually reject. A structure whose length field overruns must not
// come back as a partly-filled struct that a caller reads a MAC out of.
func TestTruncatedDataIsRefusedRatherThanPartlyDecoded(t *testing.T) {
	for _, tc := range []struct{ name, in string }{
		{"cut mid-value", realDevice[:120]},
		{"no terminator", "0002PCA0007922A0JPE1706068011104000cASY0058122A0"},
		{"far too short", "0002"},
		{"empty", ""},
	} {
		if p, err := Parse([]byte(tc.in)); err == nil {
			t.Errorf("%s: Parse succeeded and returned MAC %q, want an error",
				tc.name, p.MAC)
		}
	}
}

func TestAnUnknownFieldIsKeptRatherThanDropped(t *testing.T) {
	p, err := Parse([]byte(realDevice))
	if err != nil {
		t.Fatal(err)
	}
	// 0x11 is not in the vendor's own field table and this board carries it.
	if got := p.Fields[0x11]; got != "01.00" {
		t.Errorf("field 0x11 = %q, want %q kept even though it has no name",
			got, "01.00")
	}
}
