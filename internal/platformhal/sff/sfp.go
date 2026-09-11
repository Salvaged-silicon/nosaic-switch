package sff

// SFF-8472, the SFP/SFP+ family.
//
// Two i2c addresses: 0x50 carries identity, 0x51 carries diagnostics. The
// split is why an SFP needs two reads where a QSFP needs one, and why a module
// can be identified on a board that cannot reach 0x51 at all.
const (
	// A0h, i2c 0x50.
	sfpIdentifier = 0
	sfpVendor     = 20
	sfpPN         = 40
	sfpSN         = 68
	// sfpDiagType is byte 92: bit 6 externally calibrated, bit 5 internally
	// calibrated, bit 2 address-change-required.
	sfpDiagType = 92
	SFPA0Len    = 96

	// A2h, i2c 0x51.
	sfpTemp    = 96
	sfpVcc     = 98
	sfpTXBias  = 100
	sfpTXPower = 102
	sfpRXPower = 104
	SFPA2Len   = 112
)

// SFPHasDiagnostics is whether the module implements the A2h diagnostic page
// at all. It is optional in SFF-8472, and reading 0x51 on a module without it
// returns whatever the bus floats to -- usually 0xff, which decodes to
// plausible-looking nonsense rather than to an obvious error.
func SFPHasDiagnostics(a0 []byte) bool {
	if len(a0) <= sfpDiagType {
		return false
	}
	return a0[sfpDiagType]&0x40 != 0 || a0[sfpDiagType]&0x20 != 0
}

// SFPExternallyCalibrated modules require their raw readings to be scaled by
// per-module coefficients from A2h bytes 56..91. NOSaic does not apply them
// yet, so this says whether the numbers should be trusted.
func SFPExternallyCalibrated(a0 []byte) bool {
	if len(a0) <= sfpDiagType {
		return false
	}
	return a0[sfpDiagType]&0x10 != 0
}

// DecodeSFP reads SFF-8472. a2 may be nil for a module with no diagnostics,
// in which case identity is still returned.
func DecodeSFP(a0, a2 []byte) Module {
	m := Module{Kind: SFP}
	if len(a0) > sfpIdentifier {
		m.Identifier = a0[sfpIdentifier]
	}
	if len(a0) >= SFPA0Len {
		m.Vendor = text(a0, sfpVendor, 16)
		m.PartNumber = text(a0, sfpPN, 16)
		m.SerialNumber = text(a0, sfpSN, 16)
	}
	if len(a2) >= SFPA2Len {
		m.TempMilliC = sbe16(a2, sfpTemp) * 1000 / 256
		m.VccMV = be16(a2, sfpVcc) / 10
		m.TempOK, m.VccOK = true, true
		m.Lanes = []Lane{{
			Index:     1,
			RXPowerUW: be16(a2, sfpRXPower) / 10,
			TXBiasUA:  be16(a2, sfpTXBias) * 2,
			TXPowerUW: be16(a2, sfpTXPower) / 10,
			TXPowerOK: true,
		}}
	}
	return m
}
