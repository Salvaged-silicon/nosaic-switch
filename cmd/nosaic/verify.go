package main

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"

	nosdclient "github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
)

// verify ports and verify routes: what Linux believes, set against what the
// chip holds, read back from the chip. The same comparison and the same
// verdicts as the C CLI's cli/asic.c, so the answer does not depend on which
// switch it is asked on.

// stpName is BCM_STG_STP_*, as the chip reports it.
func stpName(s int) string {
	switch s {
	case 0:
		return "disable"
	case 1:
		return "block"
	case 2:
		return "listen"
	case 3:
		return "learn"
	case 4:
		return "forward"
	}
	return "?"
}

type linuxPort struct {
	present, up bool
	mtu         int
	addr        string
}

func linuxState(name string) linuxPort {
	ifi, err := net.InterfaceByName(name)
	if err != nil {
		return linuxPort{}
	}
	lp := linuxPort{present: true, up: ifi.Flags&net.FlagUp != 0, mtu: ifi.MTU}
	if addrs, err := ifi.Addrs(); err == nil {
		for _, a := range addrs {
			if n, ok := a.(*net.IPNet); ok && n.IP.To4() != nil {
				ones, _ := n.Mask.Size()
				lp.addr = fmt.Sprintf("%s/%d", n.IP, ones)
				break
			}
		}
	}
	return lp
}

func verifyPorts(c *nosdclient.Client) error {
	ps, err := c.ASICPorts()
	if err != nil {
		return err
	}
	fmt.Printf("%-7s %-30s %-42s %s\n", "port", "linux", "asic", "")
	bad := 0
	for _, p := range ps {
		ls := linuxState(p.Name)
		state := "down"
		switch {
		case !ls.present:
			state = "ABS"
		case ls.up:
			state = "up"
		}
		addr := ls.addr
		if addr == "" {
			addr = "-"
		}
		lin := fmt.Sprintf("%-4s mtu%-5d %-18s", state, ls.mtu, addr)
		link := map[int]string{1: "up", 0: "DOWN"}[p.Link]
		if link == "" {
			link = "?"
		}
		cpu := map[int]string{1: "yes", 0: "NO"}[p.CPUMember]
		if cpu == "" {
			cpu = "?"
		}
		asic := fmt.Sprintf("link=%-4s vlan=%-5d stp=%-7s cpu=%-3s", link, p.PVID, stpName(p.STP), cpu)

		// One verdict, most serious first. A LAG member or a switched port
		// is not a routed port: its VLAN is the LAG's or a user VLAN, the
		// CPU is in that VLAN only if it has an SVI, and spanning tree may
		// block it on purpose. Those are said, and are not faults.
		verdict, info := "", false
		blocked := p.STP >= 0 && p.STP != 4
		switch {
		case !ls.present:
			verdict = "NO LINUX INTERFACE"
		case p.Enabled == 0:
			verdict = "PORT DISABLED IN THE CHIP"
		case p.LAG != "":
			verdict, info = "member of "+p.LAG, true
			if blocked {
				verdict += ", blocked by spanning tree"
			}
		case p.Switched != 0 && blocked:
			verdict, info = fmt.Sprintf("switched, vlan %d; blocked by spanning tree", p.PVID), true
		case p.Switched != 0:
			verdict, info = fmt.Sprintf("switched, vlan %d", p.PVID), true
		case p.CPUMember == 0:
			verdict = fmt.Sprintf("CPU NOT IN VLAN %d - nothing can reach Linux", p.PVID)
		case p.WantVLAN != 0 && p.PVID != p.WantVLAN:
			verdict = fmt.Sprintf("VLAN MISMATCH - chip has %d, daemon asked for %d", p.PVID, p.WantVLAN)
		case blocked:
			verdict = fmt.Sprintf("NOT FORWARDING - stp is %s in vlan %d", stpName(p.STP), p.PVID)
		case p.Link == 0 && ls.up:
			verdict = "link down - cable or far end"
		case p.FrameMax > 0 && ls.mtu > 0 && p.FrameMax < ls.mtu:
			verdict = fmt.Sprintf("MTU %d exceeds what the chip will carry (%d)", ls.mtu, p.FrameMax)
		}
		if verdict != "" && !info && !strings.HasPrefix(verdict, "link down") {
			bad++
		}
		fmt.Printf("%-7s %-30s %-42s %s\n", p.Name, lin, asic, verdict)
	}
	fmt.Printf("\n%d port(s). \"linux\" is what the kernel has; \"asic\" is read back from the chip.\n", len(ps))
	if bad > 0 {
		return fmt.Errorf("%d disagree in a way that stops traffic", bad)
	}
	return nil
}

type rtRow struct {
	prefix, via                     string
	inLinux, inASIC, ecmp, attached bool
}

// procHex reads one of /proc/net/route's addresses. The kernel prints the
// in-memory 32-bit word as hex, so what the digits mean depends on the host's
// byte order -- the bug the C CLI's comment describes, which reversed every
// route on the PowerPC switch. Read in the host's order, it is right on both.
func procHex(h string) uint32 {
	v, _ := strconv.ParseUint(h, 16, 32)
	var b [4]byte
	binary.NativeEndian.PutUint32(b[:], uint32(v))
	return binary.BigEndian.Uint32(b[:])
}

func linuxRoutes(rows map[string]*rtRow, order *[]string) error {
	f, err := os.Open("/proc/net/route")
	if err != nil {
		return err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Scan() // the header
	for sc.Scan() {
		fs := strings.Fields(sc.Text())
		if len(fs) < 8 {
			continue
		}
		d, g, m := procHex(fs[1]), procHex(fs[2]), procHex(fs[7])
		bits := 0
		for m&0x80000000 != 0 {
			bits++
			m <<= 1
		}
		p := fmt.Sprintf("%d.%d.%d.%d/%d", d>>24, d>>16&0xff, d>>8&0xff, d&0xff, bits)
		r := rows[p]
		if r == nil {
			r = &rtRow{prefix: p}
			rows[p] = r
			*order = append(*order, p)
		}
		r.inLinux = true
		if g == 0 {
			r.attached = true
		}
		if r.via == "" {
			r.via = fs[0]
		}
	}
	return sc.Err()
}

func verifyRoutes(c *nosdclient.Client) error {
	chip, partial, err := c.ASICRoutes()
	if err != nil {
		return err
	}
	rows := map[string]*rtRow{}
	var order []string
	if err := linuxRoutes(rows, &order); err != nil {
		return err
	}
	for _, r := range chip {
		row := rows[r.Prefix]
		if row == nil {
			row = &rtRow{prefix: r.Prefix}
			rows[r.Prefix] = row
			order = append(order, r.Prefix)
		}
		row.inASIC = true
		row.ecmp = r.ECMP != 0
	}

	fmt.Printf("%-22s %-18s %-14s %s\n", "prefix", "linux", "asic", "")
	missing, onlyASIC, attached := 0, 0, 0
	for _, p := range order {
		r := rows[p]
		verdict := ""
		// Only the first case stops traffic. A route the chip has and Linux
		// does not is usually the mirror not having caught up yet.
		switch {
		case r.inLinux && !r.inASIC && r.attached:
			verdict = "directly attached - covered by host entries"
			attached++
		case r.inLinux && !r.inASIC:
			verdict = "NOT IN HARDWARE - forwarded by the CPU, if at all"
			missing++
		case !r.inLinux && r.inASIC:
			verdict = "in the chip only - stale, or not yet withdrawn"
			onlyASIC++
		}
		via := "-"
		if r.inLinux {
			via = r.via
		}
		asic := "MISSING"
		switch {
		case r.inASIC && r.ecmp:
			asic = "present ecmp"
		case r.inASIC:
			asic = "present"
		case r.attached:
			asic = "-"
		}
		fmt.Printf("%-22s %-18s %-14s %s\n", p, via, asic, verdict)
	}
	fmt.Printf("\n%d prefix(es). \"linux\" is the kernel's routing table; \"asic\" is read back from the chip's own forwarding table.\n", len(order))
	if attached > 0 {
		fmt.Printf("%d directly attached prefix(es), which the chip covers with host entries rather than routes.\n", attached)
	}
	if onlyASIC > 0 {
		fmt.Printf("%d route(s) the chip has and the kernel does not.\n", onlyASIC)
	}
	if partial {
		fmt.Println("The chip's table was only partly readable, so absences here are not conclusive.")
	}
	fmt.Println("IPv6 and per-nexthop detail are not compared: /proc/net/route carries neither.")
	if missing > 0 {
		return fmt.Errorf("%d route(s) the kernel has and the chip does not, and should", missing)
	}
	return nil
}
