package main

import (
	"fmt"
	"strings"
	"text/tabwriter"

	nosdclient "github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
)

// lagCmd states a LAG's whole configuration, the way a line in a
// configuration file does:
//
//	nosaic lag po1 lacp swp49,swp50
//	nosaic lag po1 static swp49,swp50
//	nosaic lag po1 none
//
// Like switchport, it is the end state and not a change: members the line does
// not name leave, and running it twice is the same as once. "none" removes the
// LAG and frees its members.
func lagCmd(c *nosdclient.Client, args []string) error {
	usage := fmt.Errorf("usage: nosaic lag <poN> lacp|static <port,...> | lag <poN> none")
	if len(args) < 2 {
		return usage
	}
	name := args[0]
	switch args[1] {
	case "none":
		if len(args) != 2 {
			return usage
		}
		return c.DelLAG(name)
	case "lacp", "static":
		if len(args) != 3 {
			return usage
		}
		var ports []string
		for _, p := range strings.Split(args[2], ",") {
			if p = strings.TrimSpace(p); p != "" {
				ports = append(ports, p)
			}
		}
		if err := c.AddLAG(name, args[1] == "lacp"); err != nil {
			return err
		}
		return c.SetLAGMembers(name, ports)
	}
	return usage
}

func showLAGs(c *nosdclient.Client, w *tabwriter.Writer) error {
	lags, err := c.LAGs()
	if err != nil {
		return err
	}
	if len(lags) == 0 {
		fmt.Fprintln(w, "no lags; add one with: nosaic lag po1 lacp <port,...>")
		return nil
	}
	fmt.Fprintln(w, "LAG\tMODE\tACTIVE\tINACTIVE")
	for _, l := range lags {
		mode := "static"
		if l.LACP {
			mode = "lacp"
		}
		var act, inact []string
		for _, m := range l.Members {
			if m.Active {
				act = append(act, m.Port)
			} else {
				inact = append(inact, m.Port)
			}
		}
		fmt.Fprintf(w, "%s\t%s\t%s\t%s\n", l.Name, mode, dashIfEmpty(act), dashIfEmpty(inact))
	}
	return nil
}
