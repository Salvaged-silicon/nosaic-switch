package main

import (
	"fmt"
	"os"
	"strconv"
	"syscall"
	"text/tabwriter"
	"unsafe"
)

// Raw, READ-ONLY access to one i2c bus.
//
// ⚠ THERE IS NO WRITE PATH HERE, AND THAT IS THE POINT.
//
// This exists for one job: the Nexus 3172TQ's three PCA9539 expanders carry
// the six QSFP cages' ResetL, LPMode and ModSelL lines, and nothing in this
// tree knows which bit is which. board.yml spells out the only safe way to
// find out -- read the expanders under NOSaic, boot the vendor's OS which
// drives these cages successfully, read the same registers, and diff -- and
// explicitly forbids poking at them to see what happens, because sixteen
// live outputs on an unmapped expander include, somewhere, whatever else the
// board gates.
//
// A correlation needs a reader on both sides. It does not need a writer, so
// there isn't one: the writer belongs in the board's platform driver once
// the map is known, where it can be named rather than numbered.
const (
	i2cSlave = 0x0703
	i2cSMBus = 0x0720

	smbusRead     = 1
	smbusByteData = 2
)

// ⚠ THE ADAPTER ON THIS BOARD IS SMBUS-ONLY.
//
// The obvious implementation -- write the register number, then read the
// byte -- is two plain i2c transfers, and the i801 controller these parts
// hang off does not do plain i2c transfers. It answers write(2) with
// "operation not supported", which reads like a permission or a driver
// problem rather than a missing capability. The SMBus read-byte-data
// transaction is one ioctl and is what the hardware actually implements.
type smbusData struct {
	block [34]byte // the union: byte, word and block all live here
}

type smbusIoctl struct {
	readWrite uint8
	command   uint8
	_         [2]byte // padding to the natural alignment of the uint32
	size      uint32
	data      *smbusData
}

func smbusReadByte(fd uintptr, reg byte) (byte, error) {
	var d smbusData
	a := smbusIoctl{
		readWrite: smbusRead,
		command:   reg,
		size:      smbusByteData,
		data:      &d,
	}
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, fd,
		uintptr(i2cSMBus), uintptr(unsafe.Pointer(&a))); e != 0 {
		return 0, e
	}
	return d.block[0], nil
}

func i2cReadCmd(args []string) error {
	if len(args) < 3 {
		return fmt.Errorf("usage: nosaic platform i2c <bus> <addr> <reg> [count]")
	}
	nums := make([]int, 0, 4)
	for _, a := range args {
		// Base 0: i2c addresses and register numbers are written in hex
		// everywhere they are written at all.
		v, err := strconv.ParseInt(a, 0, 32)
		if err != nil {
			return fmt.Errorf("i2c: %q is not a number", a)
		}
		nums = append(nums, int(v))
	}
	bus, addr, reg := nums[0], nums[1], nums[2]
	count := 1
	if len(nums) > 3 {
		count = nums[3]
	}
	if count < 1 || count > 256 {
		return fmt.Errorf("i2c: count must be between 1 and 256")
	}

	path := fmt.Sprintf("/dev/i2c-%d", bus)
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return fmt.Errorf("i2c: %w", err)
	}
	defer f.Close()

	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, f.Fd(),
		uintptr(i2cSlave), uintptr(addr)); e != 0 {
		return fmt.Errorf("i2c: selecting %#02x on %s: %w", addr, path, e)
	}

	w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
	defer w.Flush()
	fmt.Fprintln(w, "REG\tVALUE")
	for i := 0; i < count; i++ {
		r := byte(reg + i)
		v, err := smbusReadByte(f.Fd(), r)
		if err != nil {
			fmt.Fprintf(w, "%#02x\tERR %v\n", r, err)
			continue
		}
		fmt.Fprintf(w, "%#02x\t%#02x\n", r, v)
	}
	return nil
}
