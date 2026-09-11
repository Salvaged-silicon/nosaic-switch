package sff

// SFF-8636, the QSFP+ family.
//
// Diagnostics live in the lower half of page 00h, which is always mapped, so
// reading them needs no page select. The identifier and the vendor strings are
// in upper page 00h, which does -- offsets here are stated relative to each
// half, and the caller says which it is handing over.
const (
	// Lower page 00h, always addressable at i2c 0x50.
	qsfpIdentifier = 0
	qsfpStatus2    = 2 // bit 0: data not ready
	qsfpTemp       = 22
	qsfpVcc        = 26
	qsfpRXPower    = 34 // 4 lanes, 2 bytes each
	qsfpTXBias     = 42 // 4 lanes
	qsfpTXPower    = 50 // 4 lanes, optional
	QSFPLowerLen   = 86

	// Upper page 00h.
	qsfpUpperVendor = 148 - 128
	qsfpUpperPN     = 168 - 128
	qsfpUpperSN     = 196 - 128
	QSFPUpperLen    = 128
)

// QSFPDataReady is whether the module has finished its internal measurement
// cycle. A module read too soon after power-on reports zeros everywhere, which
// is indistinguishable from a dark link unless this bit is checked.
func QSFPDataReady(lower []byte) bool {
	if len(lower) <= qsfpStatus2 {
		return false
	}
	return lower[qsfpStatus2]&0x01 == 0
}

// DecodeQSFP reads SFF-8636 diagnostics. upper may be nil, in which case the
// vendor strings are left empty -- they need a page select and the numbers do
// not, so a caller that only wants light levels need not pay for one.
func DecodeQSFP(lower, upper []byte) Module {
	m := Module{Kind: QSFP}
	if len(lower) > qsfpIdentifier {
		m.Identifier = lower[qsfpIdentifier]
	}
	if len(lower) >= QSFPLowerLen {
		m.TempMilliC = sbe16(lower, qsfpTemp) * 1000 / 256
		m.VccMV = be16(lower, qsfpVcc) / 10 // 100 uV units -> mV
		m.TempOK, m.VccOK = true, true

		// TX power is optional. A module that does not implement it reports
		// zero, which is also what a dead laser reports, so the two are
		// separated by asking whether ANY lane is non-zero rather than by
		// trusting a capability bit that vendors set inconsistently.
		txOK := false
		for i := 0; i < 4; i++ {
			if be16(lower, qsfpTXPower+2*i) != 0 {
				txOK = true
				break
			}
		}
		for i := 0; i < 4; i++ {
			m.Lanes = append(m.Lanes, Lane{
				Index:     i + 1,
				RXPowerUW: be16(lower, qsfpRXPower+2*i) / 10, // 0.1 uW units
				TXBiasUA:  be16(lower, qsfpTXBias+2*i) * 2,   // 2 uA units
				TXPowerUW: be16(lower, qsfpTXPower+2*i) / 10,
				TXPowerOK: txOK,
			})
		}
	}
	if len(upper) >= QSFPUpperLen {
		m.Vendor = text(upper, qsfpUpperVendor, 16)
		m.PartNumber = text(upper, qsfpUpperPN, 16)
		m.SerialNumber = text(upper, qsfpUpperSN, 16)
	}
	return m
}
