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
// A correlation needs a reader on both sides. The writer came later and only
// once the map existed: the diff says exactly which registers the working OS
// sets and to what, so `i2c write` replicates a known-good state rather than
// poking to see what happens, which is what board.yml's warning is about.
// It still belongs in the board's platform driver in the end, where the bits
// can be named instead of numbered; this is the instrument that proves which
// ones to name.
const (
	i2cSlave   = 0x0703
	smbusWrite = 0
	i2cSMBus   = 0x0720

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

func smbusWriteByte(fd uintptr, reg, val byte) error {
	var d smbusData
	d.block[0] = val
	a := smbusIoctl{
		readWrite: smbusWrite,
		command:   reg,
		size:      smbusByteData,
		data:      &d,
	}
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, fd,
		uintptr(i2cSMBus), uintptr(unsafe.Pointer(&a))); e != 0 {
		return e
	}
	return nil
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
	write := false
	if len(args) > 0 && args[0] == "write" {
		write = true
		args = args[1:]
	}
	if write {
		if len(args) != 4 {
			return fmt.Errorf("usage: nosaic platform i2c write <bus> <addr> <reg> <value>")
		}
	} else if len(args) < 3 {
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
	// ⚠ nums[3] is a COUNT when reading and a VALUE when writing. Checking
	// it as a count either way rejected every write of zero -- which is a
	// perfectly ordinary value, and the error said "count must be between
	// 1 and 256" about something the caller had not written as a count.
	count := 1
	if !write {
		if len(nums) > 3 {
			count = nums[3]
		}
		if count < 1 || count > 256 {
			return fmt.Errorf("i2c: count must be between 1 and 256")
		}
	} else if nums[3] < 0 || nums[3] > 0xff {
		return fmt.Errorf("i2c: value must be a byte")
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

	if write {
		val := byte(nums[3])
		if err := smbusWriteByte(f.Fd(), byte(reg), val); err != nil {
			return fmt.Errorf("i2c: writing %#02x to %#02x reg %#02x: %w",
				val, addr, reg, err)
		}
		// Read back, because a register that ignored the write and one
		// that took it are the same call and different answers.
		back, err := smbusReadByte(f.Fd(), byte(reg))
		if err != nil {
			return fmt.Errorf("i2c: reading back: %w", err)
		}
		fmt.Fprintf(w, "REG\tWROTE\tREADS BACK\n%#02x\t%#02x\t%#02x\n",
			reg, val, back)
		return nil
	}

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
