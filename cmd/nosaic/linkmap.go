package main

import (
	"bufio"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal/scd"
)

// EPC_LINK_BMAP is the hardware bitmap that gates egress.
//
// # WHY THIS COMMAND EXISTS
//
// A port can link and carry nothing. The transmit path builds a descriptor,
// the DMA completes, tx-ok counts it -- and the frame is discarded inside the
// chip before it reaches the MAC, so no egress counter and no discard counter
// moves. Every status the SDK offers says the port is healthy.
//
// soc/common/link.c:soc_link_fwd_set maintains this memory from linkscan, and
// on this chip family it rewrites the whole bitmap on every link change. A
// port missing from it is exactly the above: silently undeliverable, with
// nothing anywhere saying so.
//
// bcm_port_link_status_get -- what `show ports` and the tap detector report --
// is SOFTWARE state. It can say up while this disagrees. That divergence is
// the thing worth measuring, and until now there was no way to read this side
// of it at all.
//
// Geometry from the chip's own description (bcm56860-memories.json):
// base 0x55880000, IPIPE, schan_block 1, acc_type 3, 4 data words, one field
// PORT_BITMAP at offset 0 width 106.
//
// ⚠ The bitmap is indexed by LOGICAL port. Established by reading it on a
// switch whose link state was known: the set bits came out as {0..49} plus
// {53,57,61,65,69} plus 105 -- the CPU, logical 1..48, and the six QSFP cage
// bases exactly. Decoding it as physical instead reported four ports that
// were carrying OSPF adjacencies at that moment as missing, which is the kind
// of confidently wrong answer this command exists to prevent.
//
// # WHAT IT MEASURED, AND WHAT THAT RETIRED
//
// On 2026-09-22 every configured port was set, including two cages with no
// module and no link. So this bitmap is NOT maintained per-link here: no bit
// is ever missing, and a missing bit therefore cannot be the silent-port
// mechanism. That was the leading theory and this is what refuted it.
//
// The command is still worth having. It is the only way to read this memory,
// it makes the claim falsifiable rather than a story about the SDK, and if a
// port ever IS absent while the interface reports up, that is the fault named
// outright.
const (
	epcLinkBmapAddr  = 0x55880000
	epcLinkBmapBits  = 106
	epcLinkBmapWords = 4
)

// decodeLinkBmap turns the raw words into the set of logical ports.
//
// Word 0 carries bits 0..31, little-endian within each word. Indexed by
// LOGICAL port -- see the note above; decoding it as physical named four
// healthy ports as faulty.
func decodeLinkBmap(words []uint32) map[int]bool {
	set := map[int]bool{}
	for bit := 0; bit < epcLinkBmapBits; bit++ {
		if bit/32 >= len(words) {
			break
		}
		if words[bit/32]>>(uint(bit)%32)&1 == 1 {
			set[bit] = true
		}
	}
	return set
}

func linkmapCmd(b *board.Board, args []string) error {
	if b.PlatformHAL.ASICPCI == "" {
		return fmt.Errorf("this board does not say where its switch chip appears")
	}
	verbose := false
	for _, a := range args {
		if a == "--verbose" || a == "-v" {
			verbose = true
		}
	}

	c, err := scd.OpenSChan(b.PlatformHAL.ASICPCI, 0)
	if err != nil {
		return err
	}
	defer c.Close()

	words, block, acc, err := readLinkBmap(c, verbose)
	if err != nil {
		return err
	}

	set := decodeLinkBmap(words)

	phys, names := portMaps()
	_ = phys

	fmt.Printf("EPC_LINK_BMAP at %#08x (block %d, acc %d)\n", epcLinkBmapAddr, block, acc)
	for i, w := range words {
		fmt.Printf("  word[%d] = %#08x\n", i, w)
	}
	fmt.Printf("\nports the chip will egress to (%d of %d bits):\n", len(set), epcLinkBmapBits)
	if len(set) == 0 {
		fmt.Println("  none -- every transmit is being discarded before the MAC")
	}

	var bits []int
	for p := range set {
		bits = append(bits, p)
	}
	sort.Ints(bits)
	for _, lg := range bits {
		if n, ok := names[lg]; ok {
			fmt.Printf("  logical %-3d  %s\n", lg, n)
		} else {
			fmt.Printf("  logical %-3d  (not a front-panel port: CPU, loopback or unused)\n", lg)
		}
	}

	// The comparison that answers the question. A port whose interface is up
	// but whose bit is clear is the silent-port fault, named rather than
	// inferred.
	fmt.Printf("\nfront-panel ports NOT in the bitmap:\n")
	missing := 0
	var lgs []int
	for lg := range names {
		lgs = append(lgs, lg)
	}
	sort.Ints(lgs)
	for _, lg := range lgs {
		if !set[lg] {
			fmt.Printf("  %-6s logical %-3d\n", names[lg], lg)
			missing++
		}
	}
	if missing == 0 {
		fmt.Println("  none -- every configured port is egress-enabled, which is")
		fmt.Println("  what this board has always shown, link or no link. A silent")
		fmt.Println("  port is therefore NOT explained by this bitmap.")
	} else {
		fmt.Printf("\nA port listed here that `nosaic show ports` calls up is the\n" +
			"silent-port fault: it will accept transmits and deliver none.\n" +
			"Restarting the datapath has cleared it every time so far.\n")
	}
	return nil
}

// readLinkBmap reads the bitmap, sweeping the header fields rather than
// trusting them.
//
// The access field is the one part of an S-Channel header that no capture
// pins down -- schanSelftest sweeps it for the same reason. A wrong value
// times out, which is a safe negative and indistinguishable from an absent
// block, so guessing once and reporting "unreadable" would be a wrong answer
// delivered confidently.
func readLinkBmap(c *scd.SChan, verbose bool) ([]uint32, uint32, uint32, error) {
	blocks := []uint32{ipipeBlock, 11}
	accs := []uint32{memACC}
	for a := uint32(0); a < 8; a++ {
		if a != memACC {
			accs = append(accs, a)
		}
	}
	if !verbose {
		c.Trace = nil
	}
	for _, blk := range blocks {
		for _, acc := range accs {
			r, err := c.Read(scd.OpcodeReadMem, blk, acc, 16, epcLinkBmapAddr,
				epcLinkBmapWords+1, 500*time.Millisecond)
			if err != nil {
				continue
			}
			if r.Failed() != nil {
				continue
			}
			// Response[0] echoes the header; the data follows it.
			if len(r.Response) < epcLinkBmapWords+1 {
				continue
			}
			w := make([]uint32, epcLinkBmapWords)
			copy(w, r.Response[1:])
			// All-zero is a legal answer only if no port has link, which is
			// not the case on a switch holding adjacencies -- so treat it as
			// the wrong header rather than as a reading.
			allZero := true
			for _, v := range w {
				if v != 0 {
					allZero = false
				}
			}
			if allZero {
				continue
			}
			return w, blk, acc, nil
		}
	}
	return nil, 0, 0, fmt.Errorf("no block answered for EPC_LINK_BMAP at %#08x under any "+
		"access field. The geometry came from the chip's memory description; if the chip "+
		"is initialised and `nosaic platform schan selftest` passes, the header shape is "+
		"what to question", epcLinkBmapAddr)
}

// portMaps reads physical->logical and logical->name from the board files the
// datapath itself uses, so this command cannot disagree with it.
func portMaps() (map[int]int, map[int]string) {
	phys := map[int]int{}
	names := map[int]string{}
	reMap := regexp.MustCompile(`^portmap_(\d+)=(\d+):`)
	reTap := regexp.MustCompile(`^tap_(et\d+)=(\d+):`)
	for _, f := range []string{"/etc/nosaic/portmap.conf", "/etc/nosaic/asic.conf",
		"/mnt/data/config/portmap.conf", "/mnt/data/config/asic.conf"} {
		fh, err := os.Open(f)
		if err != nil {
			continue
		}
		sc := bufio.NewScanner(fh)
		for sc.Scan() {
			line := strings.TrimSpace(sc.Text())
			if m := reMap.FindStringSubmatch(line); m != nil {
				lg, _ := strconv.Atoi(m[1])
				ph, _ := strconv.Atoi(m[2])
				phys[ph] = lg
			}
			if m := reTap.FindStringSubmatch(line); m != nil {
				lg, _ := strconv.Atoi(m[2])
				names[lg] = m[1]
			}
		}
		fh.Close()
	}
	return phys, names
}
