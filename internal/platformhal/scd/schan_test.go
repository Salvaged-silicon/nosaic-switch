package scd

import "testing"

// The header composition, checked against a command word captured from the
// vendor's own driver on this board.
//
// This is the one part of S-Channel that cannot be checked by reading the
// hardware: a command is overwritten by its own acknowledgement before
// anything in userspace can see it, so the encoding has to be right before the
// first transaction rather than discovered by trying. A captured word with its
// fields documented is the only ground truth there is, and it is worth pinning.
func TestHeaderMatchesTheCapturedCommandWord(t *testing.T) {
	// OPC=7 (READ_MEM_CMD), DPORT=1, ACC=3, DLEN=4.
	if got := Header(7, 1, 3, 4); got != 0x1c10c200 {
		t.Errorf("Header(7,1,3,4) = %#08x, want 0x1c10c200 as captured", got)
	}
	// A second captured pair, same command with ACC=2.
	if got := Header(7, 1, 2, 4); got != 0x1c108200 {
		t.Errorf("Header(7,1,2,4) = %#08x, want 0x1c108200 as captured", got)
	}
	// Field widths must be enforced, not merely documented: a dport of 64
	// silently wrapping into the opcode would compose a write command out of a
	// read, which is the one mistake this package must not make.
	if got := Header(7, 64, 0, 0); got != Header(7, 0, 0, 0) {
		t.Errorf("dport 64 was not masked to 6 bits: %#08x", got)
	}
	if opc := Header(0x3f, 0, 0, 0) >> 26; opc != 0x3f {
		t.Errorf("opcode field did not survive: %#x", opc)
	}
}

// Only read opcodes may be issued. A write opcode reaching the chip during
// bring-up is not recoverable, so it is refused at the API rather than left to
// a caller's care.
func TestWriteOpcodesAreRefused(t *testing.T) {
	c := &SChan{bar: make([]byte, 0x40000), base: cmc0Base}
	for _, opc := range []uint32{
		9,  // WRITE_MEM_CMD -- the one actually captured writing an L2 entry
		13, // WRITE_REG_CMD
		0,  // UNKNOWN_OPCODE
	} {
		if _, err := c.Read(opc, 57, 0, 4, 0x02030000, 2, 0); err == nil {
			t.Errorf("opcode %d was accepted; only read opcodes may be issued", opc)
		}
	}
}

// An in-flight transaction must never be stomped. The vendor OS may have been
// driving this chip moments before NOSaic touched it.
func TestABusyEngineIsNotDisturbed(t *testing.T) {
	c := &SChan{bar: make([]byte, 0x40000), base: cmc0Base}
	c.wr(schanCtrl, schanMsgStart)
	_, err := c.Read(OpcodeReadReg, 57, 0, 4, 0x02030000, 2, 0)
	if err == nil {
		t.Fatal("a transaction was started while MSG_START was already set")
	}
	if got := c.rd(schanCtrl); got != schanMsgStart {
		t.Errorf("the control register was modified to %#08x; it must be left alone", got)
	}
}

// The CMC register offsets, which the whole transaction depends on.
func TestCMCRegisterLayout(t *testing.T) {
	for _, tc := range []struct {
		cmc  int
		want int
	}{{0, 0x031000}, {1, 0x032000}, {2, 0x033000}} {
		if got := cmc0Base + tc.cmc*cmcStride; got != tc.want {
			t.Errorf("CMC%d base = %#x, want %#x", tc.cmc, got, tc.want)
		}
	}
	// MESSAGE0 at +0x00c was a correction to an earlier reading that had
	// +0x004 as the control register; +0x004 is the ack beat count.
	if schanCtrl != 0x000 || schanAck != 0x004 || schanErr != 0x008 || schanMsg0 != 0x00c {
		t.Errorf("CMC register offsets moved: ctrl=%#x ack=%#x err=%#x msg0=%#x",
			schanCtrl, schanAck, schanErr, schanMsg0)
	}
}
