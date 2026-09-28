package main

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
	"text/tabwriter"

	nosdclient "github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// vlanCmd is `nosaic vlan add|del <vid>`.
func vlanCmd(c *nosdclient.Client, args []string) error {
	usage := fmt.Errorf("usage: nosaic vlan add|del <vid>")
	if len(args) != 2 {
		return usage
	}
	vid, err := parseVID(args[1])
	if err != nil {
		return err
	}
	switch args[0] {
	case "add":
		return c.AddVLAN(vid)
	case "del":
		return c.DelVLAN(vid)
	}
	return usage
}

// sviCmd is `nosaic svi add|del <vid>`. The interface it makes is vlan<vid>,
// and takes addresses the way a port does.
func sviCmd(c *nosdclient.Client, args []string) error {
	usage := fmt.Errorf("usage: nosaic svi add|del <vid>")
	if len(args) != 2 {
		return usage
	}
	vid, err := parseVID(args[1])
	if err != nil {
		return err
	}
	switch args[0] {
	case "add":
		if err := c.AddSVI(vid); err != nil {
			return err
		}
		fmt.Println(switchapi.SVIName(vid))
		return nil
	case "del":
		return c.DelSVI(vid)
	}
	return usage
}

// switchportCmd sets a port's whole membership, the way a line in a
// configuration file states it:
//
//	nosaic switchport swp1 access 10
//	nosaic switchport swp49 trunk 10,20,30 [native 1]
//	nosaic switchport swp1 none
//
// It states the end state rather than a change, so running it twice is the
// same as once and it removes whatever the port had that the line does not
// name. That is what lets a configuration be applied over and over, which is
// how this system applies everything.
func switchportCmd(c *nosdclient.Client, args []string) error {
	usage := fmt.Errorf("usage: nosaic switchport <port> access <vid> | trunk <vid,...> [native <vid>] [tpid <0x..>] | tunnel <svid> | none")
	if len(args) < 2 {
		return usage
	}
	port := args[0]

	// A tunnel (QinQ customer) port: one statement, one membership.
	if args[1] == "tunnel" {
		if len(args) != 3 {
			return usage
		}
		vid, err := parseVID(args[2])
		if err != nil {
			return err
		}
		return c.SetPortTunnel(port, vid)
	}
	// A trunk's outer TPID, if given: the last two words of the line.
	tpid := 0
	if args[1] == "trunk" && len(args) >= 5 && args[len(args)-2] == "tpid" {
		t, err := strconv.ParseInt(args[len(args)-1], 0, 32)
		if err != nil {
			return fmt.Errorf("tpid %q is not a number (0x88a8, say)", args[len(args)-1])
		}
		tpid = int(t)
		if err := switchapi.ValidTPID(tpid); err != nil {
			return err
		}
		args = args[:len(args)-2]
	}
	want := map[int]bool{} // vid -> tagged
	switch args[1] {
	case "access":
		if len(args) != 3 {
			return usage
		}
		vid, err := parseVID(args[2])
		if err != nil {
			return err
		}
		want[vid] = false
	case "trunk":
		if len(args) != 3 && len(args) != 5 {
			return usage
		}
		for _, f := range strings.Split(args[2], ",") {
			vid, err := parseVID(f)
			if err != nil {
				return err
			}
			want[vid] = true
		}
		if len(args) == 5 {
			if args[3] != "native" {
				return usage
			}
			vid, err := parseVID(args[4])
			if err != nil {
				return err
			}
			want[vid] = false
		}
	case "none":
		if len(args) != 2 {
			return usage
		}
	default:
		return usage
	}

	vl, err := c.VLANs()
	if err != nil {
		return err
	}
	// Removals first, so a native VLAN moving elsewhere is not briefly two.
	for _, v := range vl {
		for _, m := range v.Members {
			if _, keep := want[v.VID]; m.Port == port && !keep {
				if err := c.DelPortVLAN(port, v.VID); err != nil {
					return err
				}
			}
		}
	}
	vids := make([]int, 0, len(want))
	for vid := range want {
		vids = append(vids, vid)
	}
	sort.Ints(vids)
	for _, vid := range vids {
		if err := c.SetPortVLAN(port, vid, want[vid]); err != nil {
			return err
		}
	}
	// The whole statement: a trunk line without a tpid puts it back to
	// 0x8100. Only asked of a datapath that has QinQ, so a switch without
	// it still takes an ordinary trunk.
	if tpid != 0 || (args[1] == "trunk" && c.Capabilities().QinQ) {
		return c.SetPortTPID(port, tpid)
	}
	return nil
}

func showVLANs(c *nosdclient.Client, w *tabwriter.Writer) error {
	vl, err := c.VLANs()
	if err != nil {
		return err
	}
	if len(vl) == 0 {
		fmt.Fprintln(w, "no vlans; add one with: nosaic vlan add <vid>")
		return nil
	}
	fmt.Fprintln(w, "VLAN\tSVI\tUNTAGGED\tTAGGED\tTUNNEL")
	for _, v := range vl {
		var untagged, tagged, tunnel []string
		for _, m := range v.Members {
			name := m.Port
			// A port whose outer tag is not 802.1Q's says so.
			if m.TPID != 0 && m.TPID != switchapi.DefaultTPID {
				name = fmt.Sprintf("%s(0x%04x)", m.Port, m.TPID)
			}
			switch {
			case m.Tunnel:
				tunnel = append(tunnel, m.Port)
			case m.Tagged:
				tagged = append(tagged, name)
			default:
				untagged = append(untagged, name)
			}
		}
		svi := "-"
		if v.SVI {
			svi = switchapi.SVIName(v.VID)
		}
		fmt.Fprintf(w, "%d\t%s\t%s\t%s\t%s\n", v.VID, svi, dashIfEmpty(untagged), dashIfEmpty(tagged),
			dashIfEmpty(tunnel))
	}
	return nil
}

func dashIfEmpty(s []string) string {
	if len(s) == 0 {
		return "-"
	}
	return strings.Join(s, ",")
}

func parseVID(s string) (int, error) {
	vid, err := strconv.Atoi(strings.TrimSpace(s))
	if err != nil || vid < 1 || vid > 4094 {
		return 0, fmt.Errorf("vlan %q is not a number from 1 to 4094", s)
	}
	return vid, nil
}

// verifyContract runs the switchapi conformance suite against the running
// datapath, through its socket: the same checks that gate the reference
// implementation, pointed at whatever this switch actually runs. It creates
// and removes VLAN 100 and 200 and an address on the first port, so it is for
// a switch being brought up, not one carrying traffic in those VLANs.
func verifyContract(c *nosdclient.Client) error {
	probs := switchapi.Check(c)
	for _, p := range probs {
		fmt.Printf("FAIL %v\n", p)
	}
	if len(probs) > 0 {
		return fmt.Errorf("%d conformance problem(s)", len(probs))
	}
	fmt.Println("OK the datapath meets switchapi", switchapi.Version)
	return nil
}
