package main

import (
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// scdDiff reports which of the board controller's registers are moving.
//
// # WHY THIS EXISTS
//
// A tachometer counts. Read it twice a second apart and it has changed; read
// a configuration register twice and it has not. So differencing a range of
// the board controller finds the registers that measure something, without
// knowing what any of them are called and without writing anything.
//
// That is the whole point here: this board's fans have not been found. They
// are not on the SMBus -- an exhaustive scan of both accelerators turns up
// sixteen parts and no fan controller -- which leaves the controller itself,
// and the only safe way to look inside it is to watch it.
//
// ⚠ READS ONLY, AND THAT IS NOT A STYLE CHOICE. Two earlier attempts to find
// a fan PWM register by sweeping a CPLD powered a switch off. This issues no
// write at any offset, and there is deliberately no flag that makes it.
//
// ⚠ A CHANGING REGISTER IS NOT A TACHOMETER. A free-running timer changes
// too, and so does anything counting packets, interrupts or SMBus
// transactions. Three passes are taken rather than two so that a register
// which moved once can be told from one that moves every time, but the
// output is a shortlist to investigate, not an answer.
func scdDiff(hal platformhal.HAL, args []string) error {
	return scdDiffTo(os.Stdout, hal, args)
}

type scdRegs interface {
	Read32(off int) uint32
	Len() int
}

func scdDiffTo(out io.Writer, hal platformhal.HAL, args []string) error {
	r, ok := hal.(scdRegs)
	if !ok {
		return fmt.Errorf("%w: this board has no controller to read", platformhal.ErrUnsupported)
	}
	num := func(s string) (int, error) {
		if strings.HasPrefix(s, "0x") || strings.HasPrefix(s, "0X") {
			v, err := strconv.ParseInt(s[2:], 16, 64)
			return int(v), err
		}
		v, err := strconv.ParseInt(s, 10, 64)
		return int(v), err
	}
	if len(args) < 2 {
		return fmt.Errorf("usage: nosaic platform scd diff <start> <end>\n" +
			"  byte offsets, hex accepted; reads the range three times and\n" +
			"  reports the words that changed. Reads only.")
	}
	start, err := num(args[0])
	if err != nil {
		return fmt.Errorf("start %q: %w", args[0], err)
	}
	end, err := num(args[1])
	if err != nil {
		return fmt.Errorf("end %q: %w", args[1], err)
	}
	if start < 0 || end <= start || end > r.Len() {
		return fmt.Errorf("range %#x..%#x does not fit the %d-byte BAR", start, end, r.Len())
	}
	start &^= 3
	end &^= 3

	const passes = 3
	var snap [passes][]uint32
	for p := 0; p < passes; p++ {
		s := make([]uint32, 0, (end-start)/4)
		for off := start; off < end; off += 4 {
			s = append(s, r.Read32(off))
		}
		snap[p] = s
		if p != passes-1 {
			time.Sleep(time.Second)
		}
	}

	always, sometimes := 0, 0
	for i := range snap[0] {
		a, b, c := snap[0][i], snap[1][i], snap[2][i]
		if a == b && b == c {
			continue
		}
		off := start + i*4
		if a != b && b != c {
			fmt.Fprintf(out, "%#06x  %08x -> %08x -> %08x   moves every pass\n", off, a, b, c)
			always++
			continue
		}
		fmt.Fprintf(out, "%#06x  %08x -> %08x -> %08x   moved once\n", off, a, b, c)
		sometimes++
	}
	fmt.Fprintf(out, "\n%d word(s) move every pass, %d moved once, over %#x..%#x.\n",
		always, sometimes, start, end)
	if always == 0 {
		fmt.Fprintln(out, "Nothing in this range is counting. If the fans are in here they are\n"+
			"either stopped or their tachometers are somewhere else.")
	}
	return nil
}
