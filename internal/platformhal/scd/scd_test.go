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
	if err := w.Arm(50_000); err != nil {
		t.Fatalf("arm: %v", err)
	}
	armed, timeout, _ := w.Armed()
	if !armed || timeout != 50_000 {
		t.Errorf("armed=%v timeout=%d, want true/50000", armed, timeout)
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

// The timeout lives in bits [28:16] in units of 100 ms on this SCD revision,
// NOT in the low 16 bits where Arista's GPL driver writes it. That was
// established by measurement: arming with 500 in the high field power-cycled
// the board in 40-50 s, matching 50 s, where the low half's value would have
// meant ten times that.
//
// This test exists because the GPL layout is the obvious-looking one and
// writing it would leave the real timeout at whatever it already held --
// arming that reports success and protects nothing.
func TestTheTimeoutIsInTheHighFieldInDeciseconds(t *testing.T) {
	s := fake(0x8000)
	// Aboot leaves a value in the low half; it must survive.
	s.write32(watchdogReg, 0x1770)
	w, _ := s.Watchdog()

	if err := w.Arm(50_000); err != nil { // 50 s = 500 deciseconds
		t.Fatalf("arm: %v", err)
	}
	v := s.read32(watchdogReg)
	if got := (v >> wdTimeoutShift) & wdTimeoutMax; got != 500 {
		t.Errorf("high field is %d, want 500 deciseconds", got)
	}
	if got := v & wdLowMask; got != 0x1770 {
		t.Errorf("low half is %#x, want the preserved 0x1770", got)
	}
	// The exact value measured on the board, for good measure.
	if v != 0xc1f41770 {
		t.Errorf("register is %#08x, want 0xc1f41770 as measured live", v)
	}
}

// A timeout the register cannot express must be refused rather than rounded.
// Rounding down hands back a shorter window than was asked for, and a window
// expiring early power-cycles the board out from under whoever is working on
// it.
func TestUnrepresentableTimeoutsAreRefused(t *testing.T) {
	w, _ := fake(0x8000).Watchdog()
	for _, ms := range []int{0, -1, 50, 5550, 900_000} {
		if err := w.Arm(ms); err == nil {
			t.Errorf("Arm(%d) was accepted; it cannot be represented exactly", ms)
		}
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
