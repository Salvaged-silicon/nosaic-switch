package scd

import "fmt"

// SMBusReadReg reads one register from one device on one of the SCD's SMBus
// accelerators.
//
// Reads only, and exported for diagnosis rather than for drivers: a driver
// that needs this bus should describe what it is talking to, as the thermal
// sensors and the transceiver cages do. This exists so a person can ask
// whether a part the board's own description claims is present actually
// answers, without adding a driver for a device nobody has confirmed is
// populated.
//
// That is not a hypothetical distinction on this board. Its FDL declares a
// DS125BR401 repeater at 0x58 and an exhaustive scan once concluded the
// opposite, which is a question about the hardware in front of you and not
// about any code.
func (s *SCD) SMBusReadReg(accel, bus, addr, reg int) (byte, error) {
	if accel < 0 || bus < 0 || addr < 0x08 || addr > 0x77 || reg < 0 || reg > 0xff {
		return 0, fmt.Errorf("smbus read %d/%d/%#02x reg %#02x: out of range",
			accel, bus, addr, reg)
	}
	return s.smb().ReadReg(accel, bus, addr, reg)
}
