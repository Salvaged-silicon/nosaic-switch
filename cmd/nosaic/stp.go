package main

import (
	"fmt"
	"strconv"
	"text/tabwriter"

	nosdclient "github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// stpCmd states the spanning tree's configuration, the way a configuration
// line does:
//
//	nosaic stp on [priority <n>]
//	nosaic stp off
//	nosaic stp port <port> [edge] [cost <n>]
//
// Each is the end state, not a change: "stp on" without a priority is the
// default priority, and "stp port swp1" with nothing after it puts swp1 back
// to a non-edge port with the cost its speed gives it.
func stpCmd(c *nosdclient.Client, args []string) error {
	usage := fmt.Errorf("usage: nosaic stp on [priority <n>] [hello <s>] [forward-delay <s>] [max-age <s>] | " +
		"stp off | stp port <port> [edge] [cost <n>] [priority <n>]")
	if len(args) < 1 {
		return usage
	}
	switch args[0] {
	case "on", "off":
		cfg := switchapi.STPConfig{Enabled: args[0] == "on", Priority: switchapi.STPDefaultPriority}
		rest := args[1:]
		if len(rest)%2 != 0 || (!cfg.Enabled && len(rest) != 0) {
			return usage
		}
		for i := 0; i+1 < len(rest); i += 2 {
			n, err := strconv.Atoi(rest[i+1])
			if err != nil {
				return fmt.Errorf("stp %s %q is not a number", rest[i], rest[i+1])
			}
			switch rest[i] {
			case "priority":
				cfg.Priority = n
			case "hello":
				cfg.HelloTime = n
			case "forward-delay":
				cfg.ForwardDelay = n
			case "max-age":
				cfg.MaxAge = n
			default:
				return usage
			}
		}
		return c.SetSTP(cfg)
	case "port":
		if len(args) < 2 {
			return usage
		}
		var cfg switchapi.STPPortConfig
		for i := 2; i < len(args); i++ {
			switch {
			case args[i] == "edge":
				cfg.Edge = true
			case args[i] == "cost" && i+1 < len(args):
				n, err := strconv.Atoi(args[i+1])
				if err != nil {
					return fmt.Errorf("stp cost %q is not a number", args[i+1])
				}
				cfg.Cost = n
				i++
			case args[i] == "priority" && i+1 < len(args):
				n, err := strconv.Atoi(args[i+1])
				if err != nil {
					return fmt.Errorf("stp port priority %q is not a number", args[i+1])
				}
				cfg.Priority = n
				i++
			default:
				return usage
			}
		}
		return c.SetSTPPort(args[1], cfg)
	}
	return usage
}

func showSTP(c *nosdclient.Client, w *tabwriter.Writer) error {
	st, err := c.STP()
	if err != nil {
		return err
	}
	if !st.Enabled {
		fmt.Fprintln(w, "spanning tree is off; every switched port forwards. Turn it on with: nosaic stp on")
		return nil
	}
	fmt.Fprintf(w, "bridge\t%s\tpriority %d\n", st.BridgeID, st.Priority)
	if st.RootPort == "" {
		fmt.Fprintf(w, "root\t%s\tthis bridge\n", st.RootID)
	} else {
		fmt.Fprintf(w, "root\t%s\tcost %d via %s\n", st.RootID, st.RootCost, st.RootPort)
	}
	fmt.Fprintf(w, "times\thello %d s, forward delay %d s, max age %d s\t\n", st.HelloTime, st.ForwardDelay, st.MaxAge)
	fmt.Fprintf(w, "topology changes\t%d\t\n\n", st.TopologyChanges)
	if len(st.Ports) == 0 {
		fmt.Fprintln(w, "no switched ports; spanning tree runs on ports and LAGs in a VLAN")
		return nil
	}
	fmt.Fprintln(w, "PORT\tROLE\tSTATE\tCOST\tPRIORITY\tEDGE")
	for _, p := range st.Ports {
		edge := "-"
		if p.Edge {
			edge = "edge"
		}
		fmt.Fprintf(w, "%s\t%s\t%s\t%d\t%d\t%s\n", p.Port, p.Role, p.State, p.Cost, p.Priority, edge)
	}
	return nil
}
