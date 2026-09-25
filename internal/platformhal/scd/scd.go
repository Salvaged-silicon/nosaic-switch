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
//
// ⚠ THESE ARE A DEFAULT, NOT A CONSTANT. A board states its own in
// `platform_hal: switch_reset_bits:`, because they are not the same on two
// boards that share this controller -- the 7150S-52's are 1, 2 and 8, and a
// driver carrying the numbers below writes bit 0, which does not exist there,
// and never touches two lines that do.
const (
	bitSwitchCore = 0
	bitSwitchPCIe = 1
)

// switchResetBits is what this board actually uses: its own, or the pair
// above when it has not said.
func (s *SCD) switchResetBits() []int {
	if len(s.resetBits) > 0 {
		return s.resetBits
	}
	return []int{bitSwitchCore, bitSwitchPCIe}
}

// switchResetMask is those bits as one word, for the assert and for testing
// whether the chip is held.
func (s *SCD) switchResetMask() uint32 {
	var m uint32
	for _, b := range s.switchResetBits() {
		m |= 1 << uint(b)
	}
	return m
}

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

	// smbusMap is where this board's sensors and fan controller sit. Nil
	// where the board states none, which is an answer rather than a default:
	// see platformhal.SMBusMap.
	smbusMap *platformhal.SMBusMap

	// cages is the board's front-panel transceiver table. Nil where the board
	// states none, which is an answer: this board has no cages to drive.
	cages *platformhal.CageTable

	// resets are the board's own reset lines, released during bring-up
	// alongside the switch chip's.
	resets []platformhal.ResetLine
	// resetBits is this board's switch-chip reset bits; empty means the
	// default pair. See switchResetBits().
	resetBits []int
	// alwaysPulse drives the switch resets even when they read clear.
	alwaysPulse bool

	// lamps is the board's chassis-lamp map, loaded once on first use from a
	// generated file. Cached including the failure: a board without the map
	// should say so quickly every time rather than stat a missing file on
	// every pass of the thermal loop.
	lamps *lampMap

	// Trace is optional; nil means say nothing.
	Trace Trace
}

func (s *SCD) trace(f string, a ...any) {
	if s.Trace != nil {
		s.Trace(f, a...)
	}
}

// Open maps the SCD's BAR0.
//
// cfg carries the SCD's own PCI address, the address the switch chip will
// appear at once released, and where the board's sensors and fan controller
// sit on the SMBus. The last of those has no default on purpose -- see the
// note on SMBusMap.
func Open(cfg platformhal.Config) (*SCD, error) {
	pciAddr, asicAddr := cfg.PCI, cfg.ASICPCI
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
		smbusMap: cfg.SMBus, cages: cfg.Cages, resets: cfg.Resets,
		resetBits:   cfg.SwitchResetBits,
		alwaysPulse: cfg.SwitchResetAlwaysPulse,
		close:       func() error { munmapFile(bar); return f.Close() },
	}, nil
}

// releaseBoardResets clears the board's own reset lines -- anything it holds
// besides the switch chip.
//
// Nothing is released that the board has not named. These are bits on the FPGA
// that owns every reset line on the machine, and a bit nobody has accounted
// for is not one to write on the strength of a pattern.
func (s *SCD) releaseBoardResets(before uint32) {
	for _, r := range s.resets {
		if before&(1<<r.Bit) == 0 {
			s.trace("board reset %q (bit %d) is already released", r.Name, r.Bit)
			continue
		}
		s.write32(resetClear, 1<<r.Bit)
		s.trace("released board reset %q (bit %d): %#08x",
			r.Name, r.Bit, s.read32(resetBase))
	}
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

	// The board's other reset lines first.
	//
	// ⚠ RELEASED BEFORE THE CHIP, NOT AFTER. What these gate on this board is
	// a retimer sitting between the ASIC's SerDes and two of the QSFP cages;
	// it has to be passing signal by the time the datapath trains those ports,
	// and the datapath starts as soon as the chip appears.
	//
	// Held ones only: writing the clear port for a line already released is
	// harmless, but saying so in the log makes it look like this did something.
	s.releaseBoardResets(before)

	// An already-released chip is not an error, and must not be driven
	// through the release sequence again: the writes would be no-ops, the
	// register would not change, and the check below would report a mapping
	// problem on hardware that is working. Skip to enabling it.
	held := before & s.switchResetMask()
	if held == 0 && !s.alwaysPulse {
		s.trace("both resets are already released; enabling only")
		return s.enableAndWait(ctx)
	}
	if held == 0 {
		// ⚠ MEASURED ON THE 7150S-52, 2026-09-25. Finding the bits clear
		// does NOT mean the chip has been brought out of reset -- it reads
		// the same whether the edge ever happened. On that board a chip
		// whose resets merely read clear answers 0 to every register,
		// including PIN_STRAP, which is a hardware strap that cannot be 0
		// on a live part. Driving the bits and letting them go makes it
		// answer 0x208, every time, within a second.
		//
		// So on a board that says so, the shortcut above is skipped and
		// the full sequence runs. The write to the SET port below is what
		// creates the edge; without it there is nothing to release.
		s.trace("resets already read clear, but this board wants the edge anyway")
	}

	// ⚠ ASSERT BEFORE RELEASING. The chip wants an EDGE, not an absence.
	//
	// This used to clear the held bits and stop, on the reasoning that a bit
	// already set means the chip is already in reset and setting it again
	// achieves nothing. The 7150S-52 says otherwise, and the evidence is in
	// the vendor's own boot log: at the moment its chip appears, the root port
	// logs a link DOWN and then a link UP 104 ms apart --
	//
	//     pcielw 0000:00:04.0:pcie04: link down
	//     pcielw 0000:00:04.0:pcie04: link up
	//     pci 0000:02:00.0: [8086:155b] type 00 class 0x020000
	//
	// -- and on a port with nothing attached there is no link to lose. That
	// pair is the endpoint being taken down and brought back, not arriving.
	// Arista's own board code does the same thing explicitly: it reads the SET
	// port, sets every reset field, writes it, and only then reads the CLEAR
	// port and releases them.
	//
	// Costs nothing where it was already right: this runs only in the branch
	// where the chip is held, so on a board whose reset is genuinely asserted
	// the write is a no-op and the release that follows is unchanged.
	s.write32(resetSet, s.switchResetMask())
	s.trace("asserted switch resets %#08x: %#08x", s.switchResetMask(), s.read32(resetBase))
	if err := sleepCtx(ctx, pcieResetDelay); err != nil {
		return err
	}

	// Released one at a time, in the order the board gave, with the same wait
	// between them. The order is the board's to state: on the sibling it is
	// core then PCIe and the gap is what that chip wants, and a board that
	// needs a different order says so by listing its bits differently.
	bits := s.switchResetBits()
	for i, b := range bits {
		s.write32(resetClear, 1<<uint(b))
		s.trace("after clearing bit %d: %#08x", b, s.read32(resetBase))
		if i != len(bits)-1 {
			if err := sleepCtx(ctx, pcieResetDelay); err != nil {
				return err
			}
		}
	}
	after := s.read32(resetBase)
	s.trace("status register: %#08x", s.read32(resetStatus))

	// Only meaningful when something was actually held. On the always-pulse
	// path the register starts clear and ends clear, and that is the correct
	// outcome rather than evidence of a dead mapping -- the assert between
	// them is traced above and is what proves the writes land.
	if after == before && held != 0 {
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
