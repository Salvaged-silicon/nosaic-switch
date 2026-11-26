/* SPDX-License-Identifier: Apache-2.0 */

package main

import (
	"fmt"
	"text/tabwriter"

	nosdclient "github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
)

func showLLDPCmd(c *nosdclient.Client, w *tabwriter.Writer) error {
	nbrs, err := c.LLDP()
	if err != nil {
		return err
	}
	if len(nbrs) == 0 {
		fmt.Fprintln(w, "no LLDP neighbors")
		return nil
	}
	fmt.Fprintln(w, "LOCAL\tNEIGHBOR\tREMOTE PORT\tCHASSIS\tMGMT IP\tTTL")
	for _, n := range nbrs {
		name := n.SysName
		if name == "" {
			name = n.SysDesc
		}
		if name == "" {
			name = "-"
		}
		mgmt := n.Mgmt
		if mgmt == "" {
			mgmt = "-"
		}
		fmt.Fprintf(w, "%s\t%s\t%s\t%s\t%s\t%ds\n",
			n.Local, name, n.Port, n.Chassis, mgmt, n.TTL)
	}
	return nil
}

func lldpCmd(c *nosdclient.Client, args []string) error {
	usage := fmt.Errorf("usage: nosaic lldp on|off | nosaic lldp <iface> on|off")
	switch len(args) {
	case 1:
		if args[0] != "on" && args[0] != "off" {
			return usage
		}
		return c.SetLLDP("", args[0] == "on")
	case 2:
		if args[1] != "on" && args[1] != "off" {
			return usage
		}
		return c.SetLLDP(args[0], args[1] == "on")
	default:
		return usage
	}
}
