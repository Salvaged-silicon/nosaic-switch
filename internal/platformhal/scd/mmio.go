package scd

import (
	"encoding/binary"
	"fmt"
	"os"
	"syscall"
)

// Register access against a memory-mapped BAR.
//
// Every access is bounds-checked. The lab rule this enforces was learned the
// hard way on this hardware: a blind MMIO sweep reset the box twice, and an
// out-of-range write to an FPGA that owns the reset lines and the fan
// controller is not a class of mistake worth being one typo away from.

func (s *SCD) read32(off int) uint32 {
	if err := s.check(off); err != nil {
		panic(err)
	}
	return binary.LittleEndian.Uint32(s.bar[off : off+4])
}

func (s *SCD) write32(off int, v uint32) {
	if err := s.check(off); err != nil {
		panic(err)
	}
	binary.LittleEndian.PutUint32(s.bar[off:off+4], v)
}

func (s *SCD) check(off int) error {
	if off < 0 || off+4 > len(s.bar) {
		return fmt.Errorf("SCD register %#x is outside the %d-byte BAR", off, len(s.bar))
	}
	if off%4 != 0 {
		// The FPGA answers 32-bit accesses. An unaligned one does not fail
		// cleanly; it returns or writes something else.
		return fmt.Errorf("SCD register %#x is not 4-byte aligned", off)
	}
	return nil
}

func mmapFile(f *os.File, size int) ([]byte, error) {
	if size <= 0 {
		return nil, fmt.Errorf("BAR has no size")
	}
	return syscall.Mmap(int(f.Fd()), 0, size,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
}

func munmapFile(b []byte) {
	if len(b) > 0 {
		_ = syscall.Munmap(b)
	}
}
