// Package scd drives Arista's System Control Device.
//
// The SCD is an FPGA on the PCI bus that owns the parts of an Arista switch
// that are not the forwarding chip: reset lines, GPIO, LEDs, the watchdog, and
// the SMBus reaching the fan controller and transceivers. On this hardware it
// also holds the switch ASIC in reset from power-on, so nothing about the
// datapath is reachable until the SCD has been told to let go.
//
// # WHERE THESE REGISTERS COME FROM
//
// Arista publishes sonic-platform-modules-arista under GPLv2, written by the
// authors of the EOS driver for the same FPGA. The offsets and bit meanings
// here are from that source, not from disassembly or from watching a bus --
// which is worth stating because it is much stronger evidence, and because it
// means this file can be maintained by reading their tree rather than by
// re-deriving it.
//
// The layout is architecturally fixed across Arista platforms: the switch
// reset block is at 0x4000 on all fourteen platforms in that tree, from
// Trident2 to Tomahawk4, and the watchdog is at 0x0120 on this board and on
// the 7150. Only the bit assignments vary by platform.
package scd

import (
	"context"
	"fmt"
	"os"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// Register offsets within the SCD's BAR0.
const (
	// resetBase is the switch chip's reset block. Reading it gives the
	// current state, where a set bit means held in reset.
	resetBase = 0x4000
	// resetSet asserts a reset; resetClear releases it. Two write-only ports
	// onto the same block, so a release is a single write of the bit rather
	// than a read-modify-write that could race with the hardware.
	resetSet    = resetBase + 0x00
	resetClear  = resetBase + 0x10
	resetStatus = resetBase + 0x20

	// watchdogReg is the board watchdog. Architecturally fixed across Arista
	// platforms, the same register the 7150 uses.
	watchdogReg = 0x0120
)

// Reset bits within the switch reset block, for PortolaSPlus.
//
// These are the board's, not the GPL tree's. That tree has no PortolaSPlus
// entry, and other platforms in it put the PCIe reset on bit 2 -- taking that
// number would be silently fatal here, because bit 2 is unimplemented on this
// board and unimplemented bits read as 1. Clearing it would do nothing, the
// chip would never enumerate, and ResetState would report it held in reset for
// ever while the code that wrote the bit reported success.
//
// What is attested is a live read of 0x4000 with the ASIC demonstrably
// running: 0xfffffffc. Exactly bits 0 and 1 are cleared, so those two are the
// core/PCIe pair every platform declares.
//
// Which of the two is core and which is PCIe is NOT established. That is safe
// for releasing -- both end up cleared and only the 500 ms between them is
// ordered -- and it is why ResetState's answer is honest about being a guess
// while ReleaseSwitchChip's is not.
const (
	bitSwitchCore = 0
	bitSwitchPCIe = 1
)

// Watchdog register fields.
//
// The timeout is the low 16 bits, in units of 10 ms -- which is where Arista's
// GPL driver puts it, and this driver got it wrong for a while by trusting a
// board note that said otherwise.
//
// That note concluded the timeout lived in bits [28:16] in units of 100 ms,
// from an experiment that armed the watchdog by setting ONLY the enable bit on
// the value Aboot leaves behind. Both fields kept their existing values, so
// the result was consistent with either field being the timeout and could not
// separate them. It fired in 40-50 s, which fits bits [28:16] = 500 at 100 ms
// and equally fits the low 16 bits = 6000 at 10 ms.
//
// Measured on this board by varying the fields independently:
//
//	hi = 0,    low16 = 6000   ->  ~60 s     (hi is not the timeout)
//	hi = 0,    low16 = 12000  ->  ~120 s    (linear in low16)
//	hi = 3000, low16 = 6000   ->  ~60 s     (hi does not contribute)
//	hi = 8000, low16 = 6000   ->  ~60 s
//
// Writing bits [28:16] and expecting a timeout leaves the real one at whatever
// it already held: an arm that reports success, reads back the value it wrote,
// and fires whenever Aboot's leftover says. Which it did, twice, in the middle
// of bringing the ASIC up.
const (
	wdEnable     = 1 << 31
	wdActionMask = 0x3 << 29
	// wdActionPowerCycle is action 2: a power cycle rather than a warm reset.
	// That is what makes the watchdog a real recovery path -- a warm reset
	// leaves a wedged chip wedged.
	wdActionPowerCycle = 2 << 29

	// wdTimeoutMask is the 16-bit timeout field, and wdTimeoutUnitMS its
	// resolution.
	wdTimeoutMask   = 0xffff
	wdTimeoutUnitMS = 10

	// wdHighMask is bits [28:16]. Aboot leaves 500 there and its meaning is
	// not known, so it is preserved rather than cleared -- guessing at a field
	// on the device that power-cycles the board is not worth the tidiness.
	wdHighMask = 0x1fff << 16
)

// Timing from Arista's own SwitchChip._resetOut(). These are the hardware's
// contract rather than guesses: the chip needs the core out of reset and
// settled before its PCIe interface is released, and the bus needs time to
// enumerate before the device node appears.
const (
	pcieResetDelay = 500 * time.Millisecond
	rescanDelay    = 1 * time.Second
	asicYieldTime  = 2 * time.Second
	// waitForASIC is how long the device node is waited for. Arista's own
	// tooling allows 60 s; ten was this driver's own invention and is not
	// long enough to distinguish "slow" from "never".
	waitForASIC = 60 * time.Second
)

// barWindow is how much of BAR0 is mapped. Every register this driver knows
// is well inside it.
const barWindow = 0x10000

// Trace, if set, receives a line for each register access that changes state.
// Reset bring-up fails in ways that are indistinguishable without it.
type Trace func(string, ...any)

// SCD is one System Control Device.
type SCD struct {
	// bar is the memory-mapped BAR0.
	bar   []byte
	pci   string
	asic  string
	close func() error

	// Trace is optional; nil means say nothing.
	Trace Trace
}

func (s *SCD) trace(f string, a ...any) {
	if s.Trace != nil {
		s.Trace(f, a...)
	}
}

// Open maps the SCD's BAR0. pciAddr is the SCD's PCI address, asicAddr the
// address the switch chip will appear at once released.
func Open(pciAddr, asicAddr string) (*SCD, error) {
	path := fmt.Sprintf("/sys/bus/pci/devices/%s/resource0", pciAddr)
	f, err := os.OpenFile(path, os.O_RDWR|os.O_SYNC, 0)
	if err != nil {
		return nil, fmt.Errorf("mapping the SCD at %s: %w", pciAddr, err)
	}
	fi, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, err
	}
	size := int(fi.Size())
	if size > barWindow {
		// Only the register window is needed. Mapping less is a smaller blast
		// radius on a device that owns the reset lines.
		size = barWindow
	}
	bar, err := mmapFile(f, size)
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("mapping %s: %w", path, err)
	}
	return &SCD{
		bar: bar, pci: pciAddr, asic: asicAddr,
		close: func() error { munmapFile(bar); return f.Close() },
	}, nil
}

// Close unmaps the device.
func (s *SCD) Close() error {
	if s.close == nil {
		return nil
	}
	return s.close()
}

// ResetState reports whether a reset line is asserted.
func (s *SCD) ResetState(r platformhal.Reset) (bool, error) {
	bit, err := resetBit(r)
	if err != nil {
		return false, err
	}
	v := s.read32(resetBase)
	// A set bit means held in reset, which is the opposite of how the release
	// ports are written. Getting this backwards reads a working chip as
	// broken, so it is stated once here rather than at each call site.
	return v&(1<<bit) != 0, nil
}

// ReleaseSwitchChip takes the ASIC out of reset and waits for it to appear.
//
// Core first, then PCIe after a settle, which is the order Arista's own driver
// uses; the reverse order is what puts a chip back into reset. The delays are
// theirs too.
func (s *SCD) ReleaseSwitchChip(ctx context.Context) error {
	// Every step is recorded, because the two ways this fails need completely
	// different work and the failure looks identical from outside: either the
	// writes are not reaching the register, or they are and something else
	// gates enumeration. Without the before/after values there is no way to
	// tell which, and guessing means writing more registers on an FPGA that
	// owns the reset lines.
	before := s.read32(resetBase)
	s.trace("reset register before: %#08x", before)

	// An already-released chip is not an error, and must not be driven
	// through the release sequence again: the writes would be no-ops, the
	// register would not change, and the check below would report a mapping
	// problem on hardware that is working. Skip to enabling it.
	held := before & ((1 << bitSwitchCore) | (1 << bitSwitchPCIe))
	if held == 0 {
		s.trace("both resets are already released; enabling only")
		return s.enableAndWait(ctx)
	}

	s.write32(resetClear, 1<<bitSwitchCore)
	s.trace("after clearing core (bit %d): %#08x", bitSwitchCore, s.read32(resetBase))
	if err := sleepCtx(ctx, pcieResetDelay); err != nil {
		return err
	}
	s.write32(resetClear, 1<<bitSwitchPCIe)
	after := s.read32(resetBase)
	s.trace("after clearing pcie (bit %d): %#08x", bitSwitchPCIe, after)
	s.trace("status register: %#08x", s.read32(resetStatus))

	if after == before {
		return fmt.Errorf("the reset register did not change: it read %#08x before and "+
			"after both writes, so the writes are not reaching the device. This is a "+
			"mapping or addressing problem, not a chip problem", before)
	}
	if err := sleepCtx(ctx, rescanDelay); err != nil {
		return err
	}

	return s.enableAndWaitFrom(ctx, before)
}

// enableAndWait waits for the chip to appear and enables its memory decoding.
func (s *SCD) enableAndWait(ctx context.Context) error {
	return s.enableAndWaitFrom(ctx, s.read32(resetBase))
}

func (s *SCD) enableAndWaitFrom(ctx context.Context, before uint32) error {
	node := "/sys/bus/pci/devices/" + s.asic
	if _, err := os.Stat(node); err != nil {
		// The kernel enumerated while the device was in reset and found
		// nothing, so it must be told to look again.
		if err := os.WriteFile("/sys/bus/pci/rescan", []byte("1\n"), 0o200); err != nil {
			return fmt.Errorf("rescanning the PCI bus: %w", err)
		}
	}
	deadline := time.Now().Add(waitForASIC)
	for {
		if _, err := os.Stat(node); err == nil {
			// Arista wait after the device appears before touching it.
			if err := sleepCtx(ctx, asicYieldTime); err != nil {
				return err
			}
			// Being on the bus is not the same as answering. A device
			// enumerated by a rescan has no driver bound, so nothing has
			// called pci_enable_device and its COMMAND register is 0x0000 --
			// it decodes nothing, and every MMIO read comes back 0xffffffff,
			// which looks exactly like a chip still held in reset.
			before, after, err := enableMemorySpace(s.asic)
			if err != nil {
				return err
			}
			s.trace("pci COMMAND: %#04x -> %#04x (memory space enabled)", before, after)
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("the switch chip did not appear at %s within %s of releasing "+
				"reset. The reset register went %#08x -> %#08x, so the writes did land; "+
				"either these are not both the switch resets on this board, or something "+
				"beyond reset gates enumeration",
				s.asic, waitForASIC, before, s.read32(resetBase))
		}
		if err := sleepCtx(ctx, 100*time.Millisecond); err != nil {
			return err
		}
	}
}

func resetBit(r platformhal.Reset) (uint, error) {
	switch r {
	case platformhal.ResetSwitchCore:
		return bitSwitchCore, nil
	case platformhal.ResetSwitchPCIe:
		return bitSwitchPCIe, nil
	}
	return 0, fmt.Errorf("%w: reset %q", platformhal.ErrUnsupported, r)
}

func sleepCtx(ctx context.Context, d time.Duration) error {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-t.C:
		return nil
	}
}

// SetTrace attaches a sink for the register trace.
func (s *SCD) SetTrace(f func(string, ...any)) { s.Trace = f }
