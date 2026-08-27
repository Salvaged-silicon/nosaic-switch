// Command nosd is the datapath daemon.
//
// One process owns the chip and everything else configures it through a
// socket. On real hardware that matters because a vendor SDK holds state that
// cannot sensibly be shared between processes; the reason it is a socket
// rather than a library is that the CLI must work identically whether the
// datapath underneath is a Broadcom SDK, an in-kernel driver, or veth pairs.
//
// This binary is packaged per ASIC as nosd-<asic>, each providing the virtual
// name nosd. Units, CLI and configuration only ever say nosd.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/mem"
	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/proto"
	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/server"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
	"github.com/salvaged-silicon/nosaic-switch/internal/version"
)

func main() {
	socket := flag.String("socket", proto.SocketPath, "unix socket to listen on")
	driver := flag.String("driver", "mem", "datapath driver")
	ports := flag.Int("ports", 4, "number of front-panel ports (mem driver)")

	// The mem driver can be shaped to resemble hardware that lacks a feature.
	// That is not a testing hack: most switch silicon is missing something,
	// and being able to reproduce "a chip with no multipath" without owning
	// one is how the capability model gets exercised before it meets a real
	// board that would have found the gap the hard way.
	ecmp := flag.Bool("ecmp", true, "simulate multipath support (mem driver)")
	vlans := flag.Bool("vlans", true, "simulate VLAN support (mem driver)")
	l3 := flag.Bool("l3", true, "simulate L3 support (mem driver)")
	showVersion := flag.Bool("version", false, "print version and exit")
	flag.Parse()

	if *showVersion {
		fmt.Printf("nosd %s (%s), switch-api %s\n", version.Version, version.Commit, switchapi.Version)
		return
	}

	sw, err := open(*driver, *ports, *ecmp, *vlans, *l3)
	if err != nil {
		fmt.Fprintf(os.Stderr, "nosd: %v\n", err)
		os.Exit(1)
	}
	if err := sw.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "nosd: starting the %s datapath: %v\n", *driver, err)
		os.Exit(1)
	}
	defer sw.Close()

	if err := server.New(sw, os.Stdout).Listen(*socket); err != nil {
		fmt.Fprintf(os.Stderr, "nosd: %v\n", err)
		os.Exit(1)
	}
}

func open(driver string, ports int, ecmp, vlans, l3 bool) (switchapi.Switch, error) {
	switch driver {
	case "mem":
		// A simulated datapath that forwards nothing. It exists so the
		// northbound path — CLI, configuration, the contract — can be
		// exercised with no hardware and no privileges.
		caps := mem.DefaultCaps()
		caps.ECMP = ecmp
		if !ecmp {
			caps.MaxECMP = 1
		}
		caps.VLANs = vlans
		caps.L3 = l3
		return mem.New(mem.Config{Ports: ports, Caps: caps}), nil
	}
	return nil, fmt.Errorf("unknown driver %q (known: mem)", driver)
}
