package main

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
	"text/tabwriter"

	nosdclient "github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// lagCmd states a LAG's whole configuration, the way a line in a
// configuration file does:
//
//	nosaic lag po1 lacp swp49,swp50
//	nosaic lag po1 static swp49,swp50
//	nosaic lag po7 lacp swp49 mlag 7
//	nosaic lag po1 none
//
// Like switchport, it is the end state and not a change: members the line does
// not name leave, and running it twice is the same as once. "none" removes the
// LAG and frees its members.
func lagCmd(c *nosdclient.Client, args []string) error {
	usage := fmt.Errorf("usage: nosaic lag <poN> lacp|static <port,...> [mlag <id>] [rate fast|slow] " +
		"[mode active|passive] [port-priority <n>] | lag <poN> none")
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
		// Everything after the ports is keyword and value, in any order;
		// what a line leaves out is the default, as always.
		mlag := 0
		var opts switchapi.LAGOptions
		if len(args) < 3 || len(args)%2 != 1 {
			return usage
		}
		for i := 3; i+1 < len(args); i += 2 {
			k, v := args[i], args[i+1]
			switch k {
			case "mlag":
				n, err := strconv.Atoi(v)
				if err != nil || n < 1 {
					return fmt.Errorf("mlag id %q is not a number from 1", v)
				}
				mlag = n
			case "rate":
				opts.Rate = v
			case "mode":
				if v != "active" && v != "passive" {
					return fmt.Errorf("lacp mode %q: active or passive", v)
				}
				opts.Passive = v == "passive"
			case "port-priority":
				n, err := strconv.Atoi(v)
				if err != nil {
					return fmt.Errorf("port-priority %q is not a number", v)
				}
				opts.PortPriority = n
			default:
				return usage
			}
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
		if err := c.SetLAGMembers(name, ports); err != nil {
			return err
		}
		if err := c.SetLAGOptions(name, opts); err != nil &&
			(opts != (switchapi.LAGOptions{}) || !errors.Is(err, switchapi.ErrUnsupported)) {
			return err
		}
		// The whole configuration: no mlag keyword is MLAG id 0. A datapath
		// without MLAG has nothing to take back.
		if err := c.SetLAGMLAG(name, mlag); err != nil &&
			(mlag != 0 || !errors.Is(err, switchapi.ErrUnsupported)) {
			return err
		}
		return nil
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
	fmt.Fprintln(w, "LAG\tMODE\tACTIVE\tINACTIVE\tMLAG\tLACP")
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
		ml := "-"
		if l.MLAG != 0 {
			ml = strconv.Itoa(l.MLAG)
		}
		lacp := "-"
		if l.LACP {
			how := "active"
			if l.Options.Passive {
				how = "passive"
			}
			lacp = fmt.Sprintf("%s %s, port priority %d, system priority %d",
				l.Options.Rate, how, l.Options.PortPriority, l.SystemPriority)
		}
		fmt.Fprintf(w, "%s\t%s\t%s\t%s\t%s\t%s\n", l.Name, mode, dashIfEmpty(act), dashIfEmpty(inact), ml, lacp)
	}
	return nil
}

// lacpCmd sets the switch's LACP system priority:
//
//	nosaic lacp system-priority <n>
func lacpCmd(c *nosdclient.Client, args []string) error {
	if len(args) != 2 || args[0] != "system-priority" {
		return fmt.Errorf("usage: nosaic lacp system-priority <n>")
	}
	n, err := strconv.Atoi(args[1])
	if err != nil {
		return fmt.Errorf("system-priority %q is not a number", args[1])
	}
	return c.SetLACPSystemPriority(n)
}
