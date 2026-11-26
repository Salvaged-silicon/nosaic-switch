/* SPDX-License-Identifier: Apache-2.0 */

package s6000

import (
	"fmt"
	"os"
	"runtime"
	"sync"
	"syscall"
	"unsafe"
)

// smbus is the minimal SMBus byte-data interface needed by the S6000-ON HAL
// for parts without an upstream Linux driver (MAX6620 fan controllers, CPLDs).
// Kept behind an interface so register decoding is unit-tested without hardware.
type smbus interface {
	ReadReg(addr, reg int) (byte, error)
	WriteReg(addr, reg int, val byte) error
	Close() error
}

// openSMBus is overridden in tests.
var openSMBus = openDevSMBus

// Linux <uapi/linux/i2c-dev.h> and <uapi/linux/i2c.h> constants.
//
// We use I2C_SMBUS (SMBus byte-data) rather than I2C_RDWR because the Atom
// S1220's SCH controller (i2c_isch) is an SMBus-only adapter that does not
// implement arbitrary I2C_RDWR transactions.
const (
	i2cSlaveForce    = 0x0706
	i2cSMBus         = 0x0720
	i2cSMBusWrite    = 0
	i2cSMBusRead     = 1
	i2cSMBusByteData = 2
)

// Matches struct i2c_smbus_ioctl_data in <uapi/linux/i2c-dev.h>.
type i2cSMBusIoctlData struct {
	readWrite byte
	command   byte
	_         uint16
	size      uint32
	data      uintptr
}

type devSMBus struct {
	f   *os.File
	mu  sync.Mutex
	cur int
}

func openDevSMBus(busNum int) (smbus, error) {
	f, err := os.OpenFile(fmt.Sprintf("/dev/i2c-%d", busNum), os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("i2c-%d: %w (is CONFIG_I2C_CHARDEV enabled?)", busNum, err)
	}
	return &devSMBus{f: f, cur: -1}, nil
}

func (b *devSMBus) Close() error { return b.f.Close() }

func (b *devSMBus) selectAddr(addr int) error {
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

func (b *devSMBus) ReadReg(addr, reg int) (byte, error) {
	b.mu.Lock()
	defer b.mu.Unlock()

	if err := b.selectAddr(addr); err != nil {
		return 0, err
	}
	var buf [34]byte // union i2c_smbus_data
	args := i2cSMBusIoctlData{
		readWrite: i2cSMBusRead,
		command:   byte(reg),
		size:      i2cSMBusByteData,
		data:      uintptr(unsafe.Pointer(&buf[0])),
	}
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, b.f.Fd(),
		uintptr(i2cSMBus), uintptr(unsafe.Pointer(&args)))
	runtime.KeepAlive(&buf)
	runtime.KeepAlive(&args)
	if errno != 0 {
		return 0, fmt.Errorf("smbus read %#02x reg %#02x: %w", addr, reg, errno)
	}
	return buf[0], nil
}

func (b *devSMBus) WriteReg(addr, reg int, val byte) error {
	b.mu.Lock()
	defer b.mu.Unlock()

	if err := b.selectAddr(addr); err != nil {
		return err
	}
	var buf [34]byte
	buf[0] = val
	args := i2cSMBusIoctlData{
		readWrite: i2cSMBusWrite,
		command:   byte(reg),
		size:      i2cSMBusByteData,
		data:      uintptr(unsafe.Pointer(&buf[0])),
	}
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, b.f.Fd(),
		uintptr(i2cSMBus), uintptr(unsafe.Pointer(&args)))
	runtime.KeepAlive(&buf)
	runtime.KeepAlive(&args)
	if errno != 0 {
		return fmt.Errorf("smbus write %#02x reg %#02x=%#02x: %w", addr, reg, val, errno)
	}
	return nil
}
