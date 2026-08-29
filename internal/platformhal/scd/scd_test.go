package scd

import (
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// fake gives the driver a BAR in memory, so the register arithmetic and bit
// semantics are testable without the hardware. What it cannot test is whether
// the offsets are right; those come from Arista's GPL driver and only the
// switch can confirm them.
func fake(size int) *SCD { return &SCD{bar: make([]byte, size), asic: "0000:01:00.0"} }

func TestResetStateReadsHeldInResetAsAsserted(t *testing.T) {
	s := fake(0x8000)
	// A set bit means held in reset. Reading this backwards reports a working
	// chip as broken and vice versa.
	s.write32(resetBase, 1<<bitSwitchCore)

	// Bits 0 and 1 are the pair on this board, established from a live read of
	// 0xfffffffc with the ASIC running. Other Arista platforms put the PCIe
	// reset on bit 2, which is unimplemented here and reads as 1 for ever --
	// taking that number would mean the chip never enumerates and nothing says
	// why.
	if bitSwitchCore != 0 || bitSwitchPCIe != 1 {
		t.Fatalf("reset bits are core=%d pcie=%d, want 0 and 1 for PortolaSPlus",
			bitSwitchCore, bitSwitchPCIe)
	}

	held, err := s.ResetState(platformhal.ResetSwitchCore)
	if err != nil || !held {
		t.Errorf("core reset: got held=%v err=%v, want held", held, err)
	}
	held, err = s.ResetState(platformhal.ResetSwitchPCIe)
	if err != nil || held {
		t.Errorf("pcie reset: got held=%v err=%v, want released", held, err)
	}
}

func TestArmingTheWatchdogSelectsAPowerCycle(t *testing.T) {
	s := fake(0x8000)
	w, _ := s.Watchdog()

	if armed, _, _ := w.Armed(); armed {
		t.Fatal("a zeroed register should read as disarmed")
	}
	if err := w.Arm(60_000); err != nil {
		t.Fatalf("arm: %v", err)
	}
	armed, timeout, _ := w.Armed()
	if !armed || timeout != 60_000 {
		t.Errorf("armed=%v timeout=%d, want true/60000", armed, timeout)
	}
	// A warm reset would leave a wedged chip wedged; the whole value of this
	// watchdog is that it power-cycles.
	if v := s.read32(watchdogReg); v&wdActionMask != wdActionPowerCycle {
		t.Errorf("action bits are %#x, want a power cycle", v&wdActionMask)
	}
	if err := w.Disarm(); err != nil {
		t.Fatalf("disarm: %v", err)
	}
	if armed, _, _ := w.Armed(); armed {
		t.Error("still armed after disarm")
	}
}

// The timeout is the low 16 bits in units of 10 ms, established by measurement
// on this board rather than taken from either the GPL driver or the board
// notes -- which disagreed, and only one of them was right.
//
// The board notes said bits [28:16] at 100 ms, from an experiment that armed
// by setting only the enable bit: both fields kept the values Aboot left, so
// the 40-50 s it measured fits [28:16] = 500 at 100 ms and equally fits
// low16 = 6000 at 10 ms. Varying them independently settles it:
//
//	hi = 0,    low16 = 6000   ->  ~60 s
//	hi = 0,    low16 = 12000  ->  ~120 s
//	hi = 3000, low16 = 6000   ->  ~60 s
//
// Writing the high field leaves the real timeout at Aboot's leftover, so the
// board power-cycles on someone else's schedule while reporting the value it
// was asked for. That happened twice here, mid-bring-up.
func TestTheTimeoutIsTheLowSixteenBitsInTensOfMilliseconds(t *testing.T) {
	s := fake(0x8000)
	// The value Aboot actually leaves on this board: enable clear, bits
	// [28:16] = 500, low16 = 6000.
	s.write32(watchdogReg, 0x41f41770)
	w, _ := s.Watchdog()

	if armed, _, _ := w.Armed(); armed {
		t.Error("Aboot leaves the watchdog disarmed; this should read as disarmed")
	}

	if err := w.Arm(120_000); err != nil { // 120 s = 12000 counts of 10 ms
		t.Fatalf("arm: %v", err)
	}
	v := s.read32(watchdogReg)
	if got := v & wdTimeoutMask; got != 12000 {
		t.Errorf("low 16 bits are %d, want 12000", got)
	}
	if got := (v & wdHighMask) >> 16; got != 500 {
		t.Errorf("bits [28:16] are %d, want Aboot's 500 preserved", got)
	}
	if v != 0xc1f42ee0 {
		t.Errorf("register is %#08x, want 0xc1f42ee0", v)
	}
	// And the decode is the inverse of the encode.
	if _, ms, _ := w.Armed(); ms != 120_000 {
		t.Errorf("Armed reports %d ms, want 120000", ms)
	}
}

// A timeout the register cannot express must be refused rather than rounded.
// Rounding down hands back a shorter window than was asked for, and a window
// expiring early power-cycles the board out from under whoever is working on
// it.
func TestUnrepresentableTimeoutsAreRefused(t *testing.T) {
	w, _ := fake(0x8000).Watchdog()
	for _, ms := range []int{0, -1, 5, 1005, 700_000} {
		if err := w.Arm(ms); err == nil {
			t.Errorf("Arm(%d) was accepted; it cannot be represented exactly", ms)
		}
	}
	// 655350 ms is the longest window the 16-bit field can express.
	if err := w.Arm(655_350); err != nil {
		t.Errorf("Arm(655350) is the maximum and should be accepted: %v", err)
	}
}

func TestPettingAnUnarmedWatchdogIsAnError(t *testing.T) {
	s := fake(0x8000)
	w, _ := s.Watchdog()
	if err := w.Pet(); err == nil {
		t.Error("petting a disarmed watchdog should say so rather than appear to work")
	}
}

// An out-of-range access must not reach the FPGA. Blind MMIO on this board has
// reset it twice.
func TestRegisterAccessIsBoundsChecked(t *testing.T) {
	s := fake(0x100)
	if err := s.check(0x4000); err == nil {
		t.Error("an offset past the BAR was accepted")
	}
	if err := s.check(0x11); err == nil {
		t.Error("an unaligned offset was accepted")
	}
	if err := s.check(0x10); err != nil {
		t.Errorf("a valid offset was rejected: %v", err)
	}
}
