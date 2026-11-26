/* SPDX-License-Identifier: Apache-2.0 */

package main

import (
	"fmt"
	"net/netip"
	"sort"
	"strconv"
	"text/tabwriter"

	nosdclient "github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

func vxlanCmd(c *nosdclient.Client, args []string) error {
	usage := fmt.Errorf("usage:\n" +
		"  nosaic vxlan vtep <ipv4>\n" +
		"  nosaic vxlan vni add <vni> vlan <vid>\n" +
		"  nosaic vxlan vni add <vni> vrf <vrf> vlan <vid>\n" +
		"  nosaic vxlan vni del <vni>")
	if len(args) < 2 {
		return usage
	}
	switch args[0] {
	case "vtep":
		if len(args) != 2 {
			return usage
		}
		addr, err := netip.ParseAddr(args[1])
		if err != nil || !addr.Is4() {
			return fmt.Errorf("vtep %q is not an IPv4 address", args[1])
		}
		return c.SetVTEP(addr.String())

	case "vni":
		if len(args) < 3 {
			return usage
		}
		switch args[1] {
		case "del":
			if len(args) != 3 {
				return usage
			}
			vni, err := parseVNI(args[2])
			if err != nil {
				return err
			}
			return c.DelVNI(vni)

		case "add":
			if len(args) != 5 && len(args) != 7 {
				return usage
			}
			vni, err := parseVNI(args[2])
			if err != nil {
				return err
			}
			var vid int
			var vrf string
			var l3 bool
			for i := 3; i+1 < len(args); i += 2 {
				switch args[i] {
				case "vlan":
					n, err := strconv.Atoi(args[i+1])
					if err != nil || n < 1 || n > 4094 {
						return fmt.Errorf("vlan %q is not 1..4094", args[i+1])
					}
					vid = n
				case "vrf":
					vrf = args[i+1]
					l3 = true
				default:
					return usage
				}
			}
			if vid == 0 {
				return usage
			}
			return c.AddVNI(vni, vid, vrf, l3)
		default:
			return usage
		}
	default:
		return usage
	}
}

func parseVNI(s string) (int, error) {
	n, err := strconv.Atoi(s)
	if err != nil || n < 1 || n > 16777215 {
		return 0, fmt.Errorf("vni %q is not 1..16777215", s)
	}
	return n, nil
}

func showVXLANCmd(c *nosdclient.Client, w *tabwriter.Writer) error {
	cfg, err := c.VXLAN()
	if err != nil {
		return err
	}
	vtep := cfg.VTEP
	if vtep == "" {
		vtep = "(not set)"
	}
	fmt.Fprintf(w, "vtep\t%s\n", vtep)
	if len(cfg.VNIs) == 0 {
		fmt.Fprintln(w, "no VNIs configured")
		return nil
	}
	fmt.Fprintln(w)
	fmt.Fprintln(w, "VNI\tTYPE\tVLAN\tVRF\tPEERS\tMACS")
	vnis := append([]switchapi.VXLANVNI(nil), cfg.VNIs...)
	sort.Slice(vnis, func(i, j int) bool { return vnis[i].VNI < vnis[j].VNI })
	for _, v := range vnis {
		kind := "L2"
		vrf := "-"
		if v.L3 {
			kind = "L3"
			vrf = v.VRF
		}
		fmt.Fprintf(w, "%d\t%s\t%d\t%s\t%d\t%d\n", v.VNI, kind, v.VID, vrf, v.Peers, v.MACs)
	}
	return nil
}
