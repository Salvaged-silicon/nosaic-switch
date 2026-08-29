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
// The timeout is NOT where Arista's GPL driver writes it. That driver puts it
// in the low 16 bits; this SCD revision keeps it in bits [28:16] in units of
// 100 ms, which was established by measurement rather than read: arming with
// bits [28:16] = 500 power-cycled the board in 40-50 s, matching 50 s, where
// the low half's 6000 would have meant 600 s. Writing the GPL field would
// leave the real timeout at whatever was already there -- a caller that
// believes it is protected and is not.
const (
	wdEnable     = 1 << 31
	wdActionMask = 0x3 << 29
	// wdActionPowerCycle is action 2: a power cycle rather than a warm reset.
	// That is what makes the watchdog a real recovery path -- a warm reset
	// leaves a wedged chip wedged.
	wdActionPowerCycle = 2 << 29

	// wdTimeoutShift and wdTimeoutMax describe the 13-bit deciseconds field.
	wdTimeoutShift = 16
	wdTimeoutMax   = 0x1fff // 8191 deciseconds, about 819 s
	// wdTimeoutUnitMS is 100 ms per count.
	wdTimeoutUnitMS = 100
	// wdLowMask is the low half, whose meaning is unknown. EOS and Aboot both
	// leave a value there, so it is preserved rather than zeroed.
	wdLowMask = 0xffff
)

// Timing from Arista's own SwitchChip._resetOut(). These are the hardware's
// contract rather than guesses: the chip needs the core out of reset and
// settled before its PCIe interface is released, and the bus needs time to
// enumerate before the device node appears.
const (
	pcieResetDelay = 500 * time.Millisecond
	rescanDelay    = 1 * time.Second
	asicYieldTime  = 2 * time.Second
)

// barWindow is how much of BAR0 is mapped. Every register this driver knows
// is well inside it.
const barWindow = 0x10000

// SCD is one System Control Device.
type SCD struct {
	// bar is the memory-mapped BAR0.
	bar   []byte
	pci   string
	asic  string
	close func() error
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
	s.write32(resetClear, 1<<bitSwitchCore)
	if err := sleepCtx(ctx, pcieResetDelay); err != nil {
		return err
	}
	s.write32(resetClear, 1<<bitSwitchPCIe)
	if err := sleepCtx(ctx, rescanDelay); err != nil {
		return err
	}

	// The chip is on the bus now but the kernel does not know: it enumerated
	// while the device was still in reset and found nothing.
	if err := os.WriteFile("/sys/bus/pci/rescan", []byte("1\n"), 0o200); err != nil {
		return fmt.Errorf("rescanning the PCI bus: %w", err)
	}

	node := "/sys/bus/pci/devices/" + s.asic
	deadline := time.Now().Add(10 * time.Second)
	for {
		if _, err := os.Stat(node); err == nil {
			// Arista wait after the device appears before touching it.
			return sleepCtx(ctx, asicYieldTime)
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("the switch chip did not appear at %s after releasing reset; "+
				"check the reset state with ResetState", s.asic)
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
