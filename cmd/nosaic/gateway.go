package main

import (
	"fmt"
	"net/netip"
	"text/tabwriter"

	nosdclient "github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
)

// gatewayCmd configures virtual gateways, the shared address both switches of
// an MLAG pair answer for:
//
//	nosaic gateway mac <mac>
//	nosaic gateway add <svi> <address/len>
//	nosaic gateway del <svi> <address/len>
func gatewayCmd(c *nosdclient.Client, args []string) error {
	usage := fmt.Errorf("usage: nosaic gateway mac <mac> | gateway add|del <svi> <address/len>")
	if len(args) == 2 && args[0] == "mac" {
		return c.SetVirtualMAC(args[1])
	}
	if len(args) != 3 || (args[0] != "add" && args[0] != "del") {
		return usage
	}
	p, err := netip.ParsePrefix(args[2])
	if err != nil {
		return fmt.Errorf("%q is not an address with a prefix length", args[2])
	}
	if args[0] == "add" {
		return c.AddVirtualGateway(args[1], p)
	}
	return c.DelVirtualGateway(args[1], p)
}

func showGateways(c *nosdclient.Client, w *tabwriter.Writer) error {
	gs, err := c.VirtualGateways()
	if err != nil {
		return err
	}
	if len(gs) == 0 {
		fmt.Fprintln(w, "no virtual gateways; add one with: nosaic gateway add vlan10 10.0.10.254/24")
		return nil
	}
	fmt.Fprintln(w, "SVI\tADDRESS\tMAC")
	for _, g := range gs {
		fmt.Fprintf(w, "%s\t%s\t%s\n", g.SVI, g.Address, g.MAC)
	}
	return nil
}
