// Package as4610 is the platform HAL for the Edgecore AS4610-54T: its board
// controller, fans, power supplies, temperature sensor and transceivers.
//
// Everything this board has is on i2c, reached through Linux's i2c-dev. That
// is the first time in this tree — the Arista boards reach their sensors
// through the SCD's own SMBus accelerators, which are windows in a PCI BAR,
// and this board has neither an SCD nor a PCI bus.
//
// It is also the first board whose HAL is Go rather than C. The AS5610's is in
// cli/ because the gc toolchain has never targeted 32-bit big-endian PowerPC;
// armhf it does target, so this board runs the same CLI as every x86 one and
// its HAL lives where the contract does.
package as4610

import (
	"fmt"
	"os"
	"strings"
	"sync"
	"syscall"
	"unsafe"
)

// bus is the small amount of i2c this HAL needs, behind an interface so the
// decode above it can be tested without a switch.
//
// Deliberately not a general i2c binding. Three operations cover a CPLD, a
// temperature sensor and an EEPROM, and a fuller one would be code with no
// caller and no test.
type bus interface {
	// ReadReg reads one byte from a register (SMBus read-byte-data).
	ReadReg(addr, reg int) (byte, error)
	// WriteReg writes one byte to a register (SMBus write-byte-data).
	WriteReg(addr, reg int, v byte) error
	// ReadWord reads two bytes from a register, big-endian on the wire.
	// SMBus read-word-data is little-endian by definition and every part
	// here is big-endian, so this is a plain two-byte read rather than
	// SMBus's word transaction — which would return the halves swapped and
	// decode to a plausible wrong number.
	ReadWord(addr, reg int) (uint16, error)
	// ReadAt reads n bytes from a device with an 8-bit internal offset,
	// as a write of the offset followed by a read without releasing the bus.
	ReadAt(addr, offset, n int) ([]byte, error)
	Close() error
}

// openBus is replaced in tests. A variable rather than an injected dependency
// because the thing under test is the decode, and threading a factory through
// every constructor to say so would be more machinery than the decode has.
var openBus = openDevBus

// The ioctls, from Linux's include/uapi/linux/i2c-dev.h.
const (
	i2cSlaveForce = 0x0706
	i2cRdwr       = 0x0707
)

// I2C_SLAVE_FORCE rather than I2C_SLAVE, and it matters on this board.
//
// I2C_SLAVE refuses an address a kernel driver has claimed. Two of the
// addresses here are claimed: the LM77 has a hwmon driver bound to it and the
// board EEPROM has at24. Refusing them would mean this HAL could report a
// temperature only on a kernel that could not.
//
// The risk that guard exists for is real — two drivers interleaving
// transactions on one device — and it is not this. Everything here is a
// read, of a register whose value the kernel driver is also only reading.
// The one exception is the fan duty register on the CPLD, and nothing in the
// kernel is bound to the CPLD at all.
type devBus struct {
	f   *os.File
	mu  sync.Mutex
	cur int // the currently selected slave address, to avoid a syscall per access
}

func openDevBus(n int) (bus, error) {
	f, err := os.OpenFile(fmt.Sprintf("/dev/i2c-%d", n), os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("i2c-%d: %w (is CONFIG_I2C_CHARDEV set?)", n, err)
	}
	return &devBus{f: f, cur: -1}, nil
}

func (b *devBus) Close() error { return b.f.Close() }

func (b *devBus) selectAddr(addr int) error {
	if b.cur == addr {
		return nil
	}
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, b.f.Fd(),
		uintptr(i2cSlaveForce), uintptr(addr))
	if errno != 0 {
		return fmt.Errorf("selecting %#02x: %w", addr, errno)
	}
	b.cur = addr
	return nil
}

// i2c_msg and i2c_rdwr_ioctl_data, from the same header.
//
// ⚠ The layout differs by pointer width, which is why this is written out
// rather than assumed: buf is a pointer, so the struct is 8 bytes on the
// 32-bit ARM this board is and 16 on the x86-64 that builds it. Using a fixed
// layout would work everywhere it was tested and corrupt every transfer on the
// switch.
type i2cMsg struct {
	addr  uint16
	flags uint16
	len   uint16
	_     uint16 // padding to the pointer's alignment
	buf   uintptr
}

type i2cRdwrData struct {
	msgs  uintptr
	nmsgs uint32
	_     uint32
}

const i2cMRd = 0x0001

// ReadAt is a combined write-then-read: the offset, then the data, with a
// repeated start rather than a stop between them.
//
// Doing it as two separate transfers works on an idle bus and fails on a busy
// one, because another master — or another thread here — can address a
// different device in between and the EEPROM then serves the read from
// whatever offset it was last given. On a board where six transceivers all
// answer at 0x50 behind a mux, that is not a theoretical race.
func (b *devBus) ReadAt(addr, offset, n int) ([]byte, error) {
	b.mu.Lock()
	defer b.mu.Unlock()

	off := []byte{byte(offset)}
	out := make([]byte, n)
	msgs := [2]i2cMsg{
		{addr: uint16(addr), len: 1, buf: uintptr(unsafe.Pointer(&off[0]))},
		{addr: uint16(addr), flags: i2cMRd, len: uint16(n), buf: uintptr(unsafe.Pointer(&out[0]))},
	}
	data := i2cRdwrData{msgs: uintptr(unsafe.Pointer(&msgs[0])), nmsgs: 2}

	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, b.f.Fd(),
		uintptr(i2cRdwr), uintptr(unsafe.Pointer(&data)))
	// Keep the buffers alive across the syscall: the kernel reads through
	// the pointers above, and nothing else here refers to them afterwards.
	runtimeKeepAlive(&off, &out, &msgs)
	if errno != 0 {
		return nil, fmt.Errorf("reading %d byte(s) at %#02x offset %#02x: %w",
			n, addr, offset, errno)
	}
	return out, nil
}

func (b *devBus) ReadReg(addr, reg int) (byte, error) {
	v, err := b.ReadAt(addr, reg, 1)
	if err != nil {
		return 0, err
	}
	return v[0], nil
}

func (b *devBus) ReadWord(addr, reg int) (uint16, error) {
	v, err := b.ReadAt(addr, reg, 2)
	if err != nil {
		return 0, err
	}
	return uint16(v[0])<<8 | uint16(v[1]), nil
}

func (b *devBus) WriteReg(addr, reg int, v byte) error {
	b.mu.Lock()
	defer b.mu.Unlock()

	if err := b.selectAddr(addr); err != nil {
		return err
	}
	if _, err := b.f.Write([]byte{byte(reg), v}); err != nil {
		return fmt.Errorf("writing %#02x to %#02x register %#02x: %w", v, addr, reg, err)
	}
	return nil
}

// adapterName is what Linux calls a bus, for checking that the board's stated
// numbering is still what it was. Empty if it cannot be read, which is not an
// error: the check is a safeguard, not a requirement.
func adapterName(n int) string {
	b, err := os.ReadFile(fmt.Sprintf("/sys/class/i2c-adapter/i2c-%d/name", n))
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}
