// Package virt is a datapath made of veth pairs.
//
// It is the implementation behind the virtual board, and it is a real
// implementation rather than a stub: front-panel ports are veth interfaces
// that carry traffic, addresses and routes are installed in the kernel, and a
// packet sent into one end comes out the other. What it does not have is
// silicon, so forwarding is done by the Linux stack rather than by a chip.
//
// Its purpose is to keep main provable with no switch attached. Everything
// above it — the CLI, the configuration, the contract — is exercised for real;
// only the layer that would talk to a chip is different.
//
// Each port swpN is one end of a veth pair. The other end, swpN-p, is the
// "cable": a test harness attaches to it, or it is moved into a network
// namespace to stand in for a neighbour.
package virt

import (
	"encoding/json"
	"fmt"
	"net/netip"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// Config shapes the virtual switch.
type Config struct {
	// Ports is how many front-panel ports to create.
	Ports int
	// Peer is the suffix for the far end of each veth pair.
	Peer string
}

// Switch is a veth-backed switchapi.Switch.
type Switch struct {
	mu    sync.Mutex
	cfg   Config
	names []string
}

// New builds a virtual switch. Nothing is created until Start.
func New(cfg Config) *Switch {
	if cfg.Ports <= 0 {
		cfg.Ports = 4
	}
	if cfg.Peer == "" {
		cfg.Peer = "-p"
	}
	s := &Switch{cfg: cfg}
	for i := 1; i <= cfg.Ports; i++ {
		s.names = append(s.names, fmt.Sprintf("swp%d", i))
	}
	return s
}

// Capabilities reports honestly what this datapath does.
//
// VLANs are declared absent rather than faked. Doing them properly means a
// bridge with VLAN filtering, which changes how addresses behave on a port,
// and half-implementing them would be worse than not having them: the
// conformance suite exists precisely to stop a datapath claiming something it
// does not do.
func (s *Switch) Capabilities() switchapi.Capabilities {
	return switchapi.Capabilities{
		Contract:   switchapi.Version,
		Driver:     "virt",
		MaxPorts:   s.cfg.Ports,
		VLANs:      false,
		L2Learning: false,
		L3:         true,
		IPv6:       true,
		ECMP:       true,
		MaxECMP:    32,
		Counters:   true,
	}
}

func ipCmd(args ...string) (string, error) {
	cmd := exec.Command("ip", args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return string(out), fmt.Errorf("ip %s: %w: %s",
			strings.Join(args, " "), err, strings.TrimSpace(string(out)))
	}
	return string(out), nil
}

// Start creates the ports. Existing interfaces are left alone and reused, so
// restarting nosd does not tear down a working network — which on a switch
// would mean dropping every session you were using to fix it.
func (s *Switch) Start() error {
	s.mu.Lock()
	defer s.mu.Unlock()

	for _, name := range s.names {
		peer := name + s.cfg.Peer
		if exists(name) {
			continue
		}
		if _, err := ipCmd("link", "add", name, "type", "veth", "peer", "name", peer); err != nil {
			return fmt.Errorf("creating %s: %w", name, err)
		}
		// The far end comes up unconditionally: a link whose peer is down
		// never reports carrier, and a port that can never come up is a
		// confusing thing to hand somebody.
		if _, err := ipCmd("link", "set", peer, "up"); err != nil {
			return err
		}
	}
	return nil
}

// Close leaves the interfaces in place. Removing them would drop traffic on a
// daemon restart, and a datapath that tears down the network when it exits is
// not one anybody should run on a switch.
func (s *Switch) Close() error { return nil }

func exists(name string) bool {
	_, err := os.Stat("/sys/class/net/" + name)
	return err == nil
}

func (s *Switch) Ports() ([]switchapi.Port, error) {
	out := make([]switchapi.Port, 0, len(s.names))
	for i, n := range s.names {
		out = append(out, switchapi.Port{Name: n, Index: i})
	}
	return out, nil
}

func (s *Switch) known(name string) error {
	for _, n := range s.names {
		if n == name {
			return nil
		}
	}
	return fmt.Errorf("no such port %q", name)
}

type ipLink struct {
	IfName    string   `json:"ifname"`
	Flags     []string `json:"flags"`
	MTU       int      `json:"mtu"`
	OperState string   `json:"operstate"`
}

func (s *Switch) PortStatus(name string) (switchapi.PortStatus, error) {
	if err := s.known(name); err != nil {
		return switchapi.PortStatus{}, err
	}
	out, err := ipCmd("-j", "link", "show", name)
	if err != nil {
		return switchapi.PortStatus{}, err
	}
	var links []ipLink
	if err := json.Unmarshal([]byte(out), &links); err != nil || len(links) == 0 {
		return switchapi.PortStatus{}, fmt.Errorf("cannot read the state of %s", name)
	}
	l := links[0]

	admin := false
	for _, f := range l.Flags {
		if f == "UP" {
			admin = true
		}
	}
	return switchapi.PortStatus{
		Name:    l.IfName,
		AdminUp: admin,
		// operstate is the kernel's own view, which is what a cable fault shows
		// up in; admin state alone would report a dead link as healthy.
		OperUp:     l.OperState == "UP" || l.OperState == "UNKNOWN" && admin,
		SpeedMbps:  10000,
		FullDuplex: true,
		MTU:        l.MTU,
	}, nil
}

func (s *Switch) SetPortAdmin(name string, up bool) error {
	if err := s.known(name); err != nil {
		return err
	}
	state := "down"
	if up {
		state = "up"
	}
	_, err := ipCmd("link", "set", name, state)
	return err
}

func (s *Switch) SetPortMTU(name string, mtu int) error {
	if err := s.known(name); err != nil {
		return err
	}
	_, err := ipCmd("link", "set", name, "mtu", strconv.Itoa(mtu))
	return err
}

// PortCounters reads sysfs rather than parsing ip output: the numbers are
// there directly, and it works even when the interface is down.
func (s *Switch) PortCounters(name string) (switchapi.Counters, error) {
	if err := s.known(name); err != nil {
		return switchapi.Counters{}, err
	}
	read := func(f string) uint64 {
		b, err := os.ReadFile("/sys/class/net/" + name + "/statistics/" + f)
		if err != nil {
			return 0
		}
		v, _ := strconv.ParseUint(strings.TrimSpace(string(b)), 10, 64)
		return v
	}
	return switchapi.Counters{
		RxPackets: read("rx_packets"), TxPackets: read("tx_packets"),
		RxBytes: read("rx_bytes"), TxBytes: read("tx_bytes"),
		RxErrors: read("rx_errors"), TxErrors: read("tx_errors"),
		RxDrops: read("rx_dropped"), TxDrops: read("tx_dropped"),
	}, nil
}

// VLANs are not implemented, and say so. See Capabilities.
func (s *Switch) AddVLAN(int) error                   { return switchapi.Unsupported("vlans") }
func (s *Switch) DelVLAN(int) error                   { return switchapi.Unsupported("vlans") }
func (s *Switch) SetPortVLAN(string, int, bool) error { return switchapi.Unsupported("vlans") }
func (s *Switch) FDB() ([]switchapi.FDBEntry, error) {
	return nil, switchapi.Unsupported("l2 learning")
}

func (s *Switch) AddAddress(name string, addr netip.Prefix) error {
	if err := s.known(name); err != nil {
		return err
	}
	_, err := ipCmd("addr", "add", addr.String(), "dev", name)
	return err
}

func (s *Switch) DelAddress(name string, addr netip.Prefix) error {
	if err := s.known(name); err != nil {
		return err
	}
	_, err := ipCmd("addr", "del", addr.String(), "dev", name)
	return err
}

func (s *Switch) AddRoute(r switchapi.Route) error {
	if len(r.NextHops) == 0 {
		return fmt.Errorf("route %s has no next-hops", r.Prefix)
	}
	args := []string{"route", "replace", r.Prefix.String()}
	if len(r.NextHops) == 1 {
		args = append(args, "via", r.NextHops[0].Via.String(), "dev", r.NextHops[0].Port)
	} else {
		// Multipath. The kernel does this natively, so the capability is real
		// rather than emulated -- which is what lets the virtual board test
		// the ECMP paths of the contract honestly.
		for _, nh := range r.NextHops {
			args = append(args, "nexthop", "via", nh.Via.String(), "dev", nh.Port)
		}
	}
	_, err := ipCmd(args...)
	return err
}

func (s *Switch) DelRoute(p netip.Prefix) error {
	_, err := ipCmd("route", "del", p.String())
	return err
}

type ipRoute struct {
	Dst      string `json:"dst"`
	Gateway  string `json:"gateway"`
	Dev      string `json:"dev"`
	NextHops []struct {
		Gateway string `json:"gateway"`
		Dev     string `json:"dev"`
	} `json:"nexthops"`
}

func (s *Switch) Routes() ([]switchapi.Route, error) {
	out, err := ipCmd("-j", "route", "show")
	if err != nil {
		return nil, err
	}
	var raw []ipRoute
	if err := json.Unmarshal([]byte(out), &raw); err != nil {
		return nil, err
	}

	var routes []switchapi.Route
	for _, r := range raw {
		// "default" and directly-connected routes are the kernel's, not ones
		// installed through this interface; reporting them as ours would
		// suggest a control plane that does not exist.
		if r.Dst == "" || r.Dst == "default" || (r.Gateway == "" && len(r.NextHops) == 0) {
			continue
		}
		pfx, err := netip.ParsePrefix(withMask(r.Dst))
		if err != nil {
			continue
		}
		route := switchapi.Route{Prefix: pfx}
		if len(r.NextHops) > 0 {
			for _, nh := range r.NextHops {
				if via, err := netip.ParseAddr(nh.Gateway); err == nil {
					route.NextHops = append(route.NextHops, switchapi.NextHop{Via: via, Port: nh.Dev})
				}
			}
		} else if via, err := netip.ParseAddr(r.Gateway); err == nil {
			route.NextHops = append(route.NextHops, switchapi.NextHop{Via: via, Port: r.Dev})
		}
		if len(route.NextHops) > 0 {
			routes = append(routes, route)
		}
	}
	return routes, nil
}

// withMask supplies the prefix length ip omits for a host route.
func withMask(dst string) string {
	if strings.Contains(dst, "/") {
		return dst
	}
	if strings.Contains(dst, ":") {
		return dst + "/128"
	}
	return dst + "/32"
}
