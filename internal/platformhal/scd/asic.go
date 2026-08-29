package scd

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
)

// asicSafeWindow is how much of the switch chip's BAR0 this probe will read.
//
// Bounded deliberately. BAR0 is 256 KB, and blind sweeps of it are how this
// board has been reset twice; the first 4 KB is the window established as safe
// to read on this hardware. Reading is not writing, but on a device whose
// registers have side effects that distinction is thinner than it sounds.
const asicSafeWindow = 0x1000

// cmicDevRevID is the CMIC device/revision identity register.
//
// A single named register rather than a wider sweep. It sits outside the 4 KB
// window above, and the right response to needing one value further out is to
// name that value, not to read more of a device whose registers can have side
// effects.
//
// On this board it must read 0x0002b860: device 0xb860 is the BCM56860, and
// revision 0x02 matches what PCI configuration space reports. That makes it a
// genuine check rather than a plausible-looking number -- it is cross-checked
// against a completely separate source on the same box.
const (
	cmicDevRevID     = 0x010224
	cmicDevRevExpect = 0x0002b860
)

// ASICProbe is what the switch chip says about itself.
type ASICProbe struct {
	PCI      string
	Vendor   uint32
	Device   uint32
	Revision uint32
	BAR0     uint64
	BAR0Size uint64

	// Words is the first part of BAR0, read as 32-bit words.
	Words []uint32
	// DevRevID is CMIC_DEV_REV_ID, and DevRevOK whether it matched.
	DevRevID uint32
	DevRevOK bool

	// AllOnes reports that every word read back as 0xffffffff, which is what
	// a PCI read returns when nothing answers -- a chip in reset, a failed
	// mapping, or a device that fell off the bus. It is the difference
	// between "the chip said something uninteresting" and "there is no chip",
	// and without checking it the second looks like the first.
	AllOnes bool
}

// ProbeASIC reads the switch chip's identity and the start of its BAR0.
//
// Read-only, and the point of it is narrow: to establish that the chip answers
// MMIO at all after NOSaic released it from reset. Everything previously known
// about this ASIC was learned after a kexec from the vendor OS, which leaves
// the chip in a state a standalone boot does not reproduce -- so "it answered
// last time" is not evidence for the path NOSaic actually takes.
func (s *SCD) ProbeASIC() (*ASICProbe, error) {
	dir := filepath.Join("/sys/bus/pci/devices", s.asic)
	if _, err := os.Stat(dir); err != nil {
		return nil, fmt.Errorf("the switch chip is not on the bus at %s; "+
			"release it first with ReleaseSwitchChip", s.asic)
	}

	p := &ASICProbe{PCI: s.asic}
	p.Vendor = hexAttr(dir, "vendor")
	p.Device = hexAttr(dir, "device")
	p.Revision = hexAttr(dir, "revision")

	// resource line 0 is BAR0: start, end, flags.
	if b, err := os.ReadFile(filepath.Join(dir, "resource")); err == nil {
		if lines := strings.Split(string(b), "\n"); len(lines) > 0 {
			var start, end uint64
			if f := strings.Fields(lines[0]); len(f) >= 2 {
				start, _ = strconv.ParseUint(strings.TrimPrefix(f[0], "0x"), 16, 64)
				end, _ = strconv.ParseUint(strings.TrimPrefix(f[1], "0x"), 16, 64)
			}
			p.BAR0 = start
			if end > start {
				p.BAR0Size = end - start + 1
			}
		}
	}

	f, err := os.OpenFile(filepath.Join(dir, "resource0"), os.O_RDWR|os.O_SYNC, 0)
	if err != nil {
		return nil, fmt.Errorf("mapping the switch chip's BAR0: %w", err)
	}
	defer f.Close()

	size := asicSafeWindow
	if p.BAR0Size > 0 && uint64(size) > p.BAR0Size {
		size = int(p.BAR0Size)
	}
	bar, err := syscall.Mmap(int(f.Fd()), 0, size,
		syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		return nil, fmt.Errorf("mmap of the switch chip's BAR0: %w", err)
	}
	defer syscall.Munmap(bar)

	// The identity register, mapped and read on its own.
	if p.BAR0Size >= cmicDevRevID+4 {
		idmap, err := syscall.Mmap(int(f.Fd()), 0, cmicDevRevID+4,
			syscall.PROT_READ, syscall.MAP_SHARED)
		if err == nil {
			p.DevRevID = binary.LittleEndian.Uint32(idmap[cmicDevRevID : cmicDevRevID+4])
			p.DevRevOK = p.DevRevID == cmicDevRevExpect
			syscall.Munmap(idmap)
		}
	}

	p.AllOnes = true
	for off := 0; off+4 <= len(bar); off += 4 {
		w := binary.LittleEndian.Uint32(bar[off : off+4])
		p.Words = append(p.Words, w)
		if w != 0xffffffff {
			p.AllOnes = false
		}
	}
	return p, nil
}

func hexAttr(dir, name string) uint32 {
	s := readTrim(filepath.Join(dir, name))
	v, err := strconv.ParseUint(strings.TrimPrefix(s, "0x"), 16, 32)
	if err != nil {
		return 0
	}
	return uint32(v)
}
