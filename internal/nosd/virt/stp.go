package virt

import (
	"encoding/json"
	"fmt"
	"net/netip"
	"os/exec"
	"sort"
	"strconv"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// Spanning tree on the virtual board is the kernel bridge's own, which is
// what a Linux bridge in a namespace runs: 802.1D, not rapid spanning tree.
// It elects the same root and blocks the same loops, only more slowly, and it
// has no edge ports -- so an edge setting is kept and reported as configured,
// but the operational Edge is always false here.
//
// The timers are shortened to the smallest 802.1D allows with a consistent
// max age (hello 1 s, forward delay 4 s, max age 6 s), so a test converges in
// about eight seconds rather than thirty.

const (
	stpHello  = "100" // centiseconds
	stpFwd    = "400"
	stpMaxAge = "600"
	virtMbps  = 10000 // what a veth reports
)

type stpPortCfg map[string]switchapi.STPPortConfig

type ipBridgeDetail struct {
	IfName   string `json:"ifname"`
	Master   string `json:"master"`
	LinkInfo struct {
		InfoKind      string `json:"info_kind"`
		InfoSlaveKind string `json:"info_slave_kind"`
		InfoData      struct {
			STPState       int    `json:"stp_state"`
			HelloTime      int    `json:"hello_time"`
			ForwardDelay   int    `json:"forward_delay"`
			MaxAge         int    `json:"max_age"`
			Priority       int    `json:"priority"`
			RootID         string `json:"root_id"`
			BridgeID       string `json:"bridge_id"`
			RootPort       int    `json:"root_port"`
			RootPathCost   int    `json:"root_path_cost"`
			TopologyChange int    `json:"topology_change"`
		} `json:"info_data"`
		InfoSlaveData struct {
			State            string `json:"state"`
			Cost             int    `json:"cost"`
			Priority         int    `json:"priority"`
			No               string `json:"no"`
			DesignatedBridge string `json:"designated_bridge"`
		} `json:"info_slave_data"`
	} `json:"linkinfo"`
}

func bridgeDetails() ([]ipBridgeDetail, error) {
	out, err := exec.Command("ip", "-j", "-d", "link", "show").Output()
	if err != nil {
		return nil, fmt.Errorf("ip -j -d link show: %w", err)
	}
	var l []ipBridgeDetail
	if err := json.Unmarshal(out, &l); err != nil {
		return nil, fmt.Errorf("ip -j -d link show: %w", err)
	}
	return l, nil
}

// normID rewrites the kernel's bridge ID, "8000.2:0:0:0:0:1", as
// "8000.020000000001": the same spelling as every other datapath.
func normID(id string) string {
	pri, mac, ok := strings.Cut(id, ".")
	if !ok {
		return id
	}
	var b strings.Builder
	for _, o := range strings.Split(mac, ":") {
		v, err := strconv.ParseUint(o, 16, 8)
		if err != nil {
			return id
		}
		fmt.Fprintf(&b, "%02x", v)
	}
	p, err := strconv.ParseUint(pri, 16, 16)
	if err != nil {
		return id
	}
	return fmt.Sprintf("%04x.%s", p, b.String())
}

func (s *Switch) SetSTP(cfg switchapi.STPConfig) error {
	if !s.vlans {
		return switchapi.Unsupported("spanning tree")
	}
	if err := switchapi.ValidSTPPriority(cfg.Priority); err != nil {
		return err
	}
	if err := switchapi.ValidSTPTimes(cfg); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.ensureBridge(); err != nil {
		return err
	}
	// Configured times as given; left at zero, the shortened ones this
	// board uses so a test converges in seconds.
	hello, fwd, maxAge := stpHello, stpFwd, stpMaxAge
	if cfg.HelloTime != 0 || cfg.ForwardDelay != 0 || cfg.MaxAge != 0 {
		h, f, m := switchapi.STPTimes(cfg)
		hello, fwd, maxAge = strconv.Itoa(h*100), strconv.Itoa(f*100), strconv.Itoa(m*100)
	}
	on := "0"
	if cfg.Enabled {
		on = "1"
	}
	_, err := ipCmd("link", "set", bridgeName, "type", "bridge",
		"hello_time", hello, "forward_delay", fwd, "max_age", maxAge,
		"priority", strconv.Itoa(cfg.Priority), "stp_state", on)
	return err
}

func (s *Switch) SetSTPPort(name string, cfg switchapi.STPPortConfig) error {
	if !s.vlans {
		return switchapi.Unsupported("spanning tree")
	}
	if err := switchapi.ValidSTPCost(cfg.Cost); err != nil {
		return err
	}
	if err := switchapi.ValidSTPPortPriority(cfg.Priority); err != nil {
		return err
	}
	if s.known(name) != nil && !(s.lags && isLAG(name)) {
		return fmt.Errorf("no such port %q", name)
	}
	if m := lagMemberOf(name); m != "" {
		return fmt.Errorf("%s is a member of %s; configure %s instead", name, m, m)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.stpPorts == nil {
		s.stpPorts = stpPortCfg{}
	}
	s.stpPorts[name] = cfg
	return s.applySTPPort(name)
}

// applySTPPort sets a bridge port's cost, if it is a bridge port yet; one that
// joins later gets it from SetPortVLAN. Callers hold s.mu.
func (s *Switch) applySTPPort(name string) error {
	l, err := bridgeDetails()
	if err != nil {
		return err
	}
	for _, d := range l {
		if d.IfName != name || d.Master != bridgeName {
			continue
		}
		cost := s.stpPorts[name].Cost
		if cost == 0 {
			cost = switchapi.STPDefaultCost(virtMbps)
		}
		// The kernel bridge's port priority is 802.1D-1998's six bits: the
		// 2004 value divided by four, 128 being its 32.
		prio := s.stpPorts[name].Priority
		if prio == 0 {
			prio = 128
		}
		_, err := ipCmd("link", "set", "dev", name, "type", "bridge_slave",
			"cost", strconv.Itoa(cost), "priority", strconv.Itoa(prio/4))
		return err
	}
	return nil
}

func (s *Switch) STP() (switchapi.STPStatus, error) {
	if !s.vlans {
		return switchapi.STPStatus{}, switchapi.Unsupported("spanning tree")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	out := switchapi.STPStatus{Priority: switchapi.STPDefaultPriority}
	if !exists(bridgeName) {
		return out, nil
	}
	l, err := bridgeDetails()
	if err != nil {
		return out, err
	}
	var br *ipBridgeDetail
	for i := range l {
		if l[i].IfName == bridgeName {
			br = &l[i]
		}
	}
	if br == nil {
		return out, nil
	}
	bd := br.LinkInfo.InfoData
	out.Enabled = bd.STPState != 0
	out.Priority = bd.Priority
	out.BridgeID = normID(bd.BridgeID)
	out.RootID = normID(bd.RootID)
	out.RootCost = bd.RootPathCost
	out.TopologyChanges = bd.TopologyChange
	out.HelloTime, out.ForwardDelay, out.MaxAge = bd.HelloTime/100, bd.ForwardDelay/100, bd.MaxAge/100
	for _, d := range l {
		if d.Master != bridgeName {
			continue
		}
		sd := d.LinkInfo.InfoSlaveData
		no, _ := strconv.ParseInt(strings.TrimPrefix(sd.No, "0x"), 16, 32)
		p := switchapi.STPPort{Port: d.IfName, Cost: sd.Cost, Priority: sd.Priority * 4}
		switch sd.State {
		case "forwarding":
			p.State = "forwarding"
		case "learning":
			p.State = "learning"
		default:
			p.State = "discarding"
		}
		switch {
		case sd.State == "disabled":
			p.Role = "disabled"
		case int(no) == bd.RootPort && bd.RootPort != 0:
			p.Role = "root"
			out.RootPort = d.IfName
		case sd.State == "blocking" && normID(sd.DesignatedBridge) == out.BridgeID:
			p.Role = "backup"
		case sd.State == "blocking":
			p.Role = "alternate"
		default:
			p.Role = "designated"
		}
		if !out.Enabled {
			p.Role, p.State = "designated", "forwarding"
		}
		out.Ports = append(out.Ports, p)
	}
	sort.Slice(out.Ports, func(i, j int) bool { return out.Ports[i].Port < out.Ports[j].Port })
	return out, nil
}

// MLAG needs a chip: a Linux bond has no way to present one LACP system from
// two machines. Refused, as the capability says.
func (s *Switch) SetMLAG(switchapi.MLAGConfig) error { return switchapi.Unsupported("mlag") }
func (s *Switch) SetLAGMLAG(string, int) error       { return switchapi.Unsupported("mlag") }
func (s *Switch) MLAG() (switchapi.MLAGStatus, error) {
	return switchapi.MLAGStatus{}, switchapi.Unsupported("mlag")
}

// Virtual gateways would need a macvlan per address, answered with the
// virtual MAC; not done on the virtual board. Refused, as the capability says.
func (s *Switch) SetVirtualMAC(string) error { return switchapi.Unsupported("virtual gateway") }
func (s *Switch) AddVirtualGateway(string, netip.Prefix) error {
	return switchapi.Unsupported("virtual gateway")
}
func (s *Switch) DelVirtualGateway(string, netip.Prefix) error {
	return switchapi.Unsupported("virtual gateway")
}
func (s *Switch) VirtualGateways() ([]switchapi.VirtualGateway, error) {
	return nil, switchapi.Unsupported("virtual gateway")
}
