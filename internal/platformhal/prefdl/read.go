/* SPDX-License-Identifier: Apache-2.0 */

package prefdl

import (
	"fmt"
	"os"
	"strings"
	"syscall"
	"unsafe"
)

// ⚠ SMBUS, NOT RAW I2C, AND THAT IS NOT A STYLE CHOICE.
//
// internal/platformhal/as4610 already reads i2c EEPROMs, through I2C_RDWR --
// a write of the offset and a read, with a repeated start and no stop
// between, which is what a device behind a mux needs. This does not reuse it
// because it cannot: the PIIX4 SMBus controller these Arista boards hang
// their board devices off does not implement I2C_RDWR at all. Asking it for
// one returns "adapter does not support I2C transfers", so the shared helper
// would compile here and fail on the hardware.
//
// What it does implement is SMBus read-byte-data, one byte per transaction
// with the offset as the command byte. That is enough for an EEPROM with an
// 8-bit internal address, which is what a prefdl sits on.
const (
	i2cSlaveForce = 0x0706 // Linux include/uapi/linux/i2c-dev.h
	i2cSMBus      = 0x0720

	smbusRead     = 1
	smbusByteData = 2
)

// i2c_smbus_ioctl_data, from the same header.
//
// ⚠ Written out rather than assumed, for the reason the as4610 code gives
// about its own structs: `data` is a pointer, so the size and alignment of
// this differ between the 32-bit and 64-bit builds in this tree. A layout
// that happens to be right on the machine that compiled it is wrong on the
// switch, silently, in a way that reads plausible values from the wrong
// place.
type smbusIoctlData struct {
	readWrite uint8
	command   uint8
	_         uint16 // padding to the union pointer's alignment
	size      uint32
	data      uintptr
}

// smbusData is the union; 34 bytes is its largest member (block transfers).
type smbusData struct {
	block [34]byte
}

// ReadDevice reads `n` bytes from an SMBus EEPROM at `addr` on /dev/i2c-`bus`.
//
// One transaction per byte, because that is what the controller offers. A
// prefdl is a few hundred bytes and this runs once at start-up, so the cost
// is irrelevant and the alternative does not work here.
func ReadDevice(bus, addr, n int) ([]byte, error) {
	path := fmt.Sprintf("/dev/i2c-%d", bus)
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("%s: %w (is CONFIG_I2C_CHARDEV set?)", path, err)
	}
	defer f.Close()

	// I2C_SLAVE_FORCE rather than I2C_SLAVE: on a board where at24 may have
	// claimed the identity EEPROM, refusing it would mean NOSaic could not
	// read a board that Linux can. Every access below is a read, of a device
	// nothing writes.
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, f.Fd(),
		uintptr(i2cSlaveForce), uintptr(addr)); e != 0 {
		return nil, fmt.Errorf("selecting %#02x on %s: %w", addr, path, e)
	}

	out := make([]byte, 0, n)
	for off := 0; off < n; off++ {
		var d smbusData
		args := smbusIoctlData{
			readWrite: smbusRead,
			command:   uint8(off),
			size:      smbusByteData,
			data:      uintptr(unsafe.Pointer(&d)),
		}
		if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, f.Fd(),
			uintptr(i2cSMBus), uintptr(unsafe.Pointer(&args))); e != 0 {
			if off == 0 {
				return nil, fmt.Errorf("reading %#02x on %s: %w -- nothing "+
					"answered at that address", addr, path, e)
			}
			// A short read is not a failure: the structure is shorter than
			// the device, and Parse stops at the terminator anyway.
			break
		}
		out = append(out, d.block[0])
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("%s address %#02x returned nothing", path, addr)
	}
	return out, nil
}

// AdapterName is what Linux calls a bus, for checking that a board's stated
// numbering is still what it was. Empty if it cannot be read, which is not an
// error: the check is a safeguard, not a requirement.
func AdapterName(bus int) string {
	b, err := os.ReadFile(fmt.Sprintf("/sys/class/i2c-adapter/i2c-%d/name", bus))
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

// Read reads and decodes a prefdl from an SMBus EEPROM.
func Read(bus, addr int) (*Prefdl, error) {
	// 512 is comfortably more than any prefdl seen and still one short
	// read's worth of patience if the device is smaller.
	raw, err := ReadDevice(bus, addr, 512)
	if err != nil {
		return nil, err
	}
	return Parse(raw)
}
