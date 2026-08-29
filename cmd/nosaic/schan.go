package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal/scd"
)

// The known-answer test.
//
// TOP is a core-domain block that answers on a cold chip -- which is what
// makes it the right target: the pipeline blocks are held in reset until chip
// initialisation releases them, so a timeout from one of those would say
// nothing about whether S-Channel works.
//
// The expected value is not merely plausible. 0x0002b860 is the BCM56860 at
// revision 02, and the same identity is readable from PCI configuration space
// and from CMIC_DEV_REV_ID by two completely separate paths on this board. A
// transaction that returns it has demonstrably reached the chip.
const (
	topBlock      = 57
	topDevRevID   = 0x02030000
	topDevRevWant = 0x0002b860
)

const schanUsage = `usage: nosaic platform schan <command>

  selftest             read TOP_DEV_REV_ID and check it against the known value
  read <addr>          issue one READ_REG_CMD and print the response

options for read:
  --block N            target block id (default %d, TOP)
  --acc N              access/ring field (default %d)
  --dlen N             bytes to read (default 4)
  --words N            response words to collect (default 2)

Reads only. This issues no write opcode: a bad read of a forwarding ASIC is
recoverable and a bad write is not.
`

func schanCmd(b *board.Board, args []string) error {
	if b.PlatformHAL.ASICPCI == "" {
		return fmt.Errorf("this board does not say where its switch chip appears")
	}
	if len(args) == 0 {
		fmt.Printf(schanUsage, topBlock, defaultACC)
		return nil
	}

	c, err := scd.OpenSChan(b.PlatformHAL.ASICPCI, 0)
	if err != nil {
		return err
	}
	defer c.Close()
	c.Trace = func(f string, a ...any) { fmt.Printf("  schan: "+f+"\n", a...) }

	switch args[0] {
	case "selftest":
		return schanSelftest(c)
	case "read":
		return schanRead(c, args[1:])
	}
	return fmt.Errorf("unknown schan command %q", args[0])
}

// defaultACC is the access field used for a register read. It is the one field
// of the header that no capture pins down, so the self-test establishes it by
// trying and reports what worked rather than this being an assumption nobody
// ever checks.
const defaultACC = 5

func schanSelftest(c *scd.SChan) error {
	fmt.Printf("reading TOP_DEV_REV_ID (block %d, %#08x); expecting %#08x\n\n",
		topBlock, topDevRevID, topDevRevWant)

	// Try the expected access value first, then the rest. A wrong access field
	// produces a TIMEOUT, which the board notes record as a safe negative --
	// the transaction simply goes unanswered. Sweeping a 3-bit field with a
	// read opcode against an address known to exist is bounded and tells us
	// something no amount of reasoning will.
	order := []uint32{defaultACC}
	for a := uint32(0); a < 8; a++ {
		if a != defaultACC {
			order = append(order, a)
		}
	}

	engineRan := false
	for _, variant := range []scd.HeaderVariant{scd.VariantNarrow, scd.VariantWide} {
		for _, acc := range order {
			c.Trace = nil // one line per attempt is enough across 16 of them
			r, err := c.ReadVariant(variant, scd.OpcodeReadReg, topBlock, acc, 4,
				topDevRevID, 2, 200*time.Millisecond)
			if err != nil {
				fmt.Printf("  %-14s acc=%d  %v\n", variant, acc, err)
				continue
			}
			// A completed transaction, even a timed-out one, means the engine
			// itself is alive: it took MSG_START, ran, and reported. That is a
			// different and much better failure than an engine that never
			// responds, and it is worth separating.
			engineRan = true
			if err := r.Failed(); err != nil {
				fmt.Printf("  %-14s acc=%d  no answer (SCHAN_CTRL=%#08x)\n",
					variant, acc, r.Ctrl)
				continue
			}
			fmt.Printf("  %-14s acc=%d  MSG_DONE after %d polls, SCHAN_ERR=%#08x\n",
				variant, acc, r.Polls, r.Err)
			for i, w := range r.Response {
				fmt.Printf("      RSP[%d] = %#08x\n", i, w)
			}
			if len(r.Response) > 1 && r.Response[1] == topDevRevWant {
				fmt.Printf("\nS-Channel reaches the chip: TOP_DEV_REV_ID reads %#08x, "+
					"the BCM56860 at revision 02.\n"+
					"Header variant: %s.  Access field for a register read: %d.\n",
					r.Response[1], variant, acc)
				return nil
			}
			fmt.Printf("      (completed, but not the expected identity)\n")
		}
	}

	if engineRan {
		// Worth stating precisely, because the two halves need different work.
		return fmt.Errorf("the S-Channel engine works -- it accepted MSG_START, ran, and "+
			"reported -- but no block answered at %#08x under either header variant. "+
			"That is what a chip looks like before initialisation releases its blocks "+
			"from reset: the engine is the CPU's side of the interface and comes up with "+
			"the CMIC, while the blocks behind it do not. Chip init is the next work, "+
			"not S-Channel", topDevRevID)
	}
	return fmt.Errorf("the S-Channel engine did not complete a transaction at all; check " +
		"that the chip is released and memory-enabled with `nosaic platform asic`")
}

func schanRead(c *scd.SChan, args []string) error {
	block, acc, dlen, words := uint32(topBlock), uint32(defaultACC), uint32(4), 2
	var addrArg string
	for i := 0; i < len(args); i++ {
		next := func() string {
			if i+1 < len(args) {
				i++
				return args[i]
			}
			return ""
		}
		switch args[i] {
		case "--block":
			block = uint32(mustNum(next()))
		case "--acc":
			acc = uint32(mustNum(next()))
		case "--dlen":
			dlen = uint32(mustNum(next()))
		case "--words":
			words = mustNum(next())
		default:
			addrArg = args[i]
		}
	}
	if addrArg == "" {
		return fmt.Errorf("schan read needs an address, e.g. 0x02030000")
	}
	addr, err := strconv.ParseUint(strings.TrimPrefix(addrArg, "0x"), 16, 32)
	if err != nil {
		return fmt.Errorf("%q is not a 32-bit hex address", addrArg)
	}

	r, err := c.Read(scd.OpcodeReadReg, block, acc, dlen, uint32(addr), words, time.Second)
	if err != nil {
		return err
	}
	fmt.Printf("  SCHAN_CTRL = %#08x after %d polls\n", r.Ctrl, r.Polls)
	fmt.Printf("  SCHAN_ERR  = %#08x\n", r.Err)
	if err := r.Failed(); err != nil {
		return err
	}
	for i, w := range r.Response {
		fmt.Printf("  RSP[%d] = %#08x\n", i, w)
	}
	return nil
}

func mustNum(s string) int {
	v, err := strconv.ParseInt(strings.TrimPrefix(s, "0x"), 0, 32)
	if err != nil {
		fmt.Fprintf(os.Stderr, "nosaic: %q is not a number\n", s)
		os.Exit(2)
	}
	return int(v)
}
