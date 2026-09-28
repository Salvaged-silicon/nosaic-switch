package main

import (
	"fmt"
	"strconv"
	"text/tabwriter"

	nosdclient "github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
)

// macCmd sets the MAC aging time, the way the configuration line does:
//
//	nosaic mac aging <seconds>
//
// 0 is never; 10 to 1000000 otherwise; 300, 802.1D's default, until set.
func macCmd(c *nosdclient.Client, args []string) error {
	if len(args) != 2 || args[0] != "aging" {
		return fmt.Errorf("usage: nosaic mac aging <seconds>")
	}
	n, err := strconv.Atoi(args[1])
	if err != nil {
		return fmt.Errorf("mac aging %q is not a number", args[1])
	}
	return c.SetMACAging(n)
}

func showMAC(c *nosdclient.Client, w *tabwriter.Writer) error {
	s, err := c.MACAging()
	if err != nil {
		return err
	}
	if s == 0 {
		fmt.Fprintln(w, "aging\tnever")
	} else {
		fmt.Fprintf(w, "aging\t%d s\n", s)
	}
	return nil
}
