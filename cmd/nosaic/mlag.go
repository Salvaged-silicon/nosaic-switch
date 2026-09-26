package main

import (
	"fmt"
	"strconv"
	"text/tabwriter"

	nosdclient "github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// mlagCmd states the MLAG configuration, the way a configuration line does:
//
//	nosaic mlag on peer-link <port|poN> [peer-address <ip>] [priority <n>]
//	nosaic mlag off
//
// An MLAG interface is a LAG given an id: nosaic lag po7 lacp swp49 mlag 7.
func mlagCmd(c *nosdclient.Client, args []string) error {
	usage := fmt.Errorf("usage: nosaic mlag on peer-link <port> [peer-address <ip>] [priority <n>] " +
		"[hello <ms>] [dead <ms>] [settle <ms>] [heartbeat-port <n>] | mlag off")
	if len(args) == 1 && args[0] == "off" {
		return c.SetMLAG(switchapi.MLAGConfig{})
	}
	if len(args) < 3 || args[0] != "on" {
		return usage
	}
	cfg := switchapi.MLAGConfig{Enabled: true, Priority: switchapi.MLAGDefaultPriority}
	for i := 1; i+1 < len(args); i += 2 {
		switch args[i] {
		case "peer-link":
			cfg.PeerLink = args[i+1]
		case "peer-address":
			cfg.PeerAddress = args[i+1]
		case "priority", "hello", "dead", "settle", "heartbeat-port":
			n, err := strconv.Atoi(args[i+1])
			if err != nil {
				return fmt.Errorf("mlag %s %q is not a number", args[i], args[i+1])
			}
			switch args[i] {
			case "priority":
				cfg.Priority = n
			case "hello":
				cfg.HelloMs = n
			case "dead":
				cfg.DeadMs = n
			case "settle":
				cfg.SettleMs = n
			case "heartbeat-port":
				cfg.HeartbeatPort = n
			}
		default:
			return usage
		}
	}
	if len(args)%2 == 0 {
		return usage
	}
	return c.SetMLAG(cfg)
}

func showMLAG(c *nosdclient.Client, w *tabwriter.Writer) error {
	st, err := c.MLAG()
	if err != nil {
		return err
	}
	if !st.Enabled {
		fmt.Fprintln(w, "mlag is off. Turn it on with: nosaic mlag on peer-link <port>")
		return nil
	}
	yn := func(b bool) string {
		if b {
			return "up"
		}
		return "down"
	}
	peer := st.Peer
	if peer == "" {
		peer = "not heard"
	}
	fmt.Fprintf(w, "role\t%s\n", st.Role)
	fmt.Fprintf(w, "peer\t%s\n", peer)
	fmt.Fprintf(w, "peer-link\t%s, link %s, peer %s\n", st.PeerLink, yn(st.PeerLinkUp), map[bool]string{true: "heard", false: "not heard"}[st.PeerAlive])
	fmt.Fprintf(w, "heartbeat\t%s\n", map[bool]string{true: "heard", false: "not heard"}[st.Heartbeat])
	fmt.Fprintf(w, "lacp system\t%s\n", st.SystemID)
	fmt.Fprintf(w, "synced macs\t%d\n", st.SyncedMACs)
	fmt.Fprintf(w, "timers\thello %d ms, dead %d ms, settle %d ms, heartbeat port %d, priority %d\n\n",
		st.HelloMs, st.DeadMs, st.SettleMs, st.HeartbeatPort, st.Priority)
	if len(st.Interfaces) == 0 {
		fmt.Fprintln(w, "no mlag interfaces; make one with: nosaic lag po7 lacp <port> mlag 7")
		return nil
	}
	fmt.Fprintln(w, "ID\tLAG\tLOCAL\tPEER\tSTATE")
	for _, i := range st.Interfaces {
		fmt.Fprintf(w, "%d\t%s\t%s\t%s\t%s\n", i.ID, i.LAG, yn(i.Local), yn(i.Peer), i.State)
	}
	return nil
}
