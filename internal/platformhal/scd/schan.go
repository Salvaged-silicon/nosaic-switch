package scd

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"syscall"
	"time"
)

// S-Channel: the command/response path into the switch chip.
//
// MMIO on the ASIC's BAR reaches the CMIC, the chip's host interface. Getting
// at anything behind it -- registers in the forwarding blocks, table memories
// -- goes through S-Channel: compose a command word and an address in the
// message registers, set MSG_START, poll for MSG_DONE, read the response back
// out of the same message registers.
//
// # WHERE THESE FACTS COME FROM
//
// The register map and the command-word layout were established on this board
// by the reverse-engineering work in the private repository this port links
// to, and validated there against live silicon. What is reproduced here is the
// hardware fact -- offsets, bit positions, the transaction sequence -- and the
// code is NOSaic's own.
//
// The header layout is confirmed against a captured command word: a command
// documented as OPC=7 DPORT=1 ACC=3 DLEN=4 encodes to 0x1c10c200, which the
// composition below reproduces exactly.

// CMC block bases within the ASIC's BAR0, and the register offsets inside one.
//
// CMC0 is the active one on this board: a live read found CMC0 populated and
// CMC1 entirely zero, which is what a single-unit system looks like.
const (
	cmc0Base  = 0x031000
	cmcStride = 0x1000

	schanCtrl = 0x000
	schanAck  = 0x004
	schanErr  = 0x008
	schanMsg0 = 0x00c
	// schanMsgCount is MESSAGE0..MESSAGE22.
	schanMsgCount = 23
)

// SCHAN_CTRL bits.
const (
	schanMsgStart     = 1 << 0
	schanMsgDone      = 1 << 1
	schanAbort        = 1 << 2
	schanSERCheckFail = 1 << 20
	schanNACK         = 1 << 21
	schanTimeout      = 1 << 22
	schanError        = 1 << 23
)

// S-Channel opcodes.
//
// Only read opcodes are listed, and only read opcodes are accepted. This is
// the single most important property of this code: a bad read of a forwarding
// ASIC is recoverable and a bad write is not. Adding a write opcode here is a
// deliberate decision to be made when something actually needs one, not a
// convenience to leave lying around during bring-up.
const (
	OpcodeReadMem = 7
	OpcodeReadReg = 11
)

var readOpcodes = map[uint32]string{
	OpcodeReadMem: "READ_MEM_CMD",
	OpcodeReadReg: "READ_REG_CMD",
}

// SChan is the S-Channel interface of one switch chip.
type SChan struct {
	bar   []byte
	base  int
	close func() error
	Trace Trace
}

// OpenSChan maps the switch chip's BAR and prepares CMC0.
func OpenSChan(asicPCI string, cmc int) (*SChan, error) {
	if cmc < 0 || cmc > 2 {
		return nil, fmt.Errorf("CMC %d is out of range 0..2", cmc)
	}
	path := filepath.Join("/sys/bus/pci/devices", asicPCI, "resource0")
	f, err := os.OpenFile(path, os.O_RDWR|os.O_SYNC, 0)
	if err != nil {
		return nil, fmt.Errorf("mapping the switch chip's BAR0: %w "+
			"(is it released from reset?)", err)
	}
	fi, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, err
	}
	size := int(fi.Size())
	base := cmc0Base + cmc*cmcStride
	if base+0x100 > size {
		f.Close()
		return nil, fmt.Errorf("CMC%d at %#x is beyond the %d-byte BAR", cmc, base, size)
	}
	bar, err := syscall.Mmap(int(f.Fd()), 0, size,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("mmap of the switch chip's BAR0: %w", err)
	}
	return &SChan{
		bar: bar, base: base,
		close: func() error { syscall.Munmap(bar); return f.Close() },
	}, nil
}

// Close unmaps the chip.
func (c *SChan) Close() error {
	if c.close == nil {
		return nil
	}
	return c.close()
}

func (c *SChan) trace(f string, a ...any) {
	if c.Trace != nil {
		c.Trace(f, a...)
	}
}

func (c *SChan) rd(off int) uint32 {
	return binary.LittleEndian.Uint32(c.bar[c.base+off : c.base+off+4])
}

func (c *SChan) wr(off int, v uint32) {
	binary.LittleEndian.PutUint32(c.bar[c.base+off:c.base+off+4], v)
}

// Header composes an S-Channel command word.
//
// Verified against a captured command: OPC=7 DPORT=1 ACC=3 DLEN=4 is
// 0x1c10c200, which this reproduces.
func Header(opc, dport, acc, dlen uint32) uint32 {
	return (opc&0x3f)<<26 | (dport&0x3f)<<20 | (acc&0x07)<<14 | (dlen&0x7f)<<7
}

// HeaderWide composes the alternate header variant.
//
// The chip supports two header formats, selected by a runtime feature flag,
// and they disagree about where the block field sits: 6 bits at [25:20] as in
// Header above, or 7 bits at [25:19] here. The reverse-engineering work could
// not settle which is active -- the flag is runtime state, and a captured
// command word decodes plausibly under both.
//
// It is settled the only way it can be from outside: by issuing a read whose
// correct answer is known and seeing which variant produces it. Both are read
// opcodes against an address that exists, and a wrong variant times out
// harmlessly.
func HeaderWide(opc, dport, acc, dlen uint32) uint32 {
	return (opc&0x3f)<<26 | (dport&0x7f)<<19 | (acc&0x1f)<<14 | (dlen&0x7f)<<7
}

// HeaderVariant names a header layout.
type HeaderVariant int

const (
	// VariantNarrow puts the block field in 6 bits at [25:20].
	VariantNarrow HeaderVariant = iota
	// VariantWide puts it in 7 bits at [25:19].
	VariantWide
)

func (v HeaderVariant) compose(opc, dport, acc, dlen uint32) uint32 {
	if v == VariantWide {
		return HeaderWide(opc, dport, acc, dlen)
	}
	return Header(opc, dport, acc, dlen)
}

func (v HeaderVariant) String() string {
	if v == VariantWide {
		return "wide [25:19]"
	}
	return "narrow [25:20]"
}

// Result is one completed S-Channel transaction.
type Result struct {
	Ctrl     uint32
	Err      uint32
	Response []uint32
	// Polls is how many times the control register was read before it
	// reported done -- useful for telling "fast" from "only just made it".
	Polls int
}

// Failed reports whether the hardware flagged a problem, and what.
func (r *Result) Failed() error {
	switch {
	case r.Ctrl&schanTimeout != 0:
		return fmt.Errorf("S-Channel TIMEOUT: the addressed block did not answer "+
			"(SCHAN_CTRL=%#08x, SCHAN_ERR=%#08x). Either the block id is wrong or "+
			"that block is still held in reset", r.Ctrl, r.Err)
	case r.Ctrl&schanNACK != 0:
		return fmt.Errorf("S-Channel NACK: the block refused the command "+
			"(SCHAN_CTRL=%#08x)", r.Ctrl)
	case r.Ctrl&schanSERCheckFail != 0:
		return fmt.Errorf("S-Channel SER check failed (SCHAN_CTRL=%#08x)", r.Ctrl)
	case r.Ctrl&schanError != 0:
		return fmt.Errorf("S-Channel error (SCHAN_CTRL=%#08x, SCHAN_ERR=%#08x)", r.Ctrl, r.Err)
	}
	return nil
}

// Read issues a read command and returns the response words.
//
// Only read opcodes are accepted; anything else is refused rather than
// attempted. words is how many response words to collect.
func (c *SChan) Read(opc, dport, acc, dlen, addr uint32, words int, timeout time.Duration) (*Result, error) {
	return c.ReadVariant(VariantNarrow, opc, dport, acc, dlen, addr, words, timeout)
}

// ReadVariant is Read with an explicit header layout.
func (c *SChan) ReadVariant(v HeaderVariant, opc, dport, acc, dlen, addr uint32, words int, timeout time.Duration) (*Result, error) {
	name, ok := readOpcodes[opc]
	if !ok {
		return nil, fmt.Errorf("opcode %d is not a read opcode; this driver issues "+
			"reads only, because a bad read of a forwarding ASIC is recoverable and "+
			"a bad write is not", opc)
	}
	if words < 1 || words > schanMsgCount {
		return nil, fmt.Errorf("response length %d is outside 1..%d", words, schanMsgCount)
	}

	// Never stomp a transaction that is already running. On a board where the
	// vendor OS may have been driving this chip moments ago, that is not
	// hypothetical.
	if ctrl := c.rd(schanCtrl); ctrl&schanMsgStart != 0 {
		return nil, fmt.Errorf("S-Channel is busy: SCHAN_CTRL=%#08x has MSG_START set", ctrl)
	}

	hdr := v.compose(opc, dport, acc, dlen)
	c.trace("header %#08x  %s dport=%d acc=%d dlen=%d  (%s)", hdr, name, dport, acc, dlen, v)
	c.trace("addr   %#08x -> MESSAGE1", addr)

	c.wr(schanMsg0, hdr)
	c.wr(schanMsg0+4, addr)

	// Read the message registers back before starting.
	//
	// Without this, "MSG_DONE with TIMEOUT" is ambiguous in a way that matters:
	// it proves the engine ran, but not that it ran the command intended. A
	// BAR write that never lands produces exactly the same symptom as a block
	// that does not answer, and the two need completely different work.
	if got := c.rd(schanMsg0); got != hdr {
		return nil, fmt.Errorf("the command word did not reach MESSAGE0: wrote %#08x, "+
			"read back %#08x. The BAR mapping is not working, so nothing below this "+
			"is meaningful", hdr, got)
	}
	if got := c.rd(schanMsg0 + 4); got != addr {
		return nil, fmt.Errorf("the address did not reach MESSAGE1: wrote %#08x, "+
			"read back %#08x", addr, got)
	}
	// Clear the status bits before starting, so what is read afterwards
	// belongs to this transaction and not the last one.
	c.wr(schanCtrl, 0)
	c.wr(schanCtrl, schanMsgStart)

	r := &Result{}
	deadline := time.Now().Add(timeout)
	for {
		r.Polls++
		r.Ctrl = c.rd(schanCtrl)
		if r.Ctrl&schanMsgDone != 0 {
			break
		}
		if time.Now().After(deadline) {
			// Leaving MSG_START set would poison the next transaction.
			c.wr(schanCtrl, 0)
			return nil, fmt.Errorf("S-Channel did not complete within %s "+
				"(SCHAN_CTRL=%#08x after %d polls)", timeout, r.Ctrl, r.Polls)
		}
	}
	r.Err = c.rd(schanErr)
	for i := 0; i < words; i++ {
		r.Response = append(r.Response, c.rd(schanMsg0+4*i))
	}
	// Leave the engine idle for whoever comes next.
	c.wr(schanCtrl, 0)
	return r, nil
}
