package virt

import (
	"encoding/json"
	"fmt"
	"os/exec"
	"sort"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// LAGs on the virtual board are Linux bonds, which is the kernel's own model
// of link aggregation and speaks real LACP: a far end in a test namespace can
// run a bond of its own and the two negotiate.
//
//   - LACP is mode 802.3ad with a fast LACPDU rate, so a test converges in
//     seconds rather than the 90 a slow-rate timeout costs.
//   - Static is balance-xor: every member with link carries traffic, spread by
//     a layer-3+4 hash, which is what a static port-channel on a chip does.
//
// As with VLANs, all state lives in the kernel and is read back from it, so a
// restarted nosd finds the LAGs it made.

// bondWorks reports whether the kernel can make bonds here, by making one.
//
// Asking /sys/module/bonding was the first version, and it was wrong in the
// place that matters most: a container has no /sys/module at all, so the
// dataplane test found "no bonding" and skipped every LAG check -- on a kernel
// where creating a bond works perfectly. Asking the kernel is the only answer
// that holds in every namespace.
func bondWorks() bool {
	const probe = "nosbondprobe" // under IFNAMSIZ: 15 characters at most
	if exec.Command("ip", "link", "add", probe, "type", "bond").Run() != nil {
		return false
	}
	_ = exec.Command("ip", "link", "del", probe).Run()
	return true
}

type ipLinkDetail struct {
	IfName   string `json:"ifname"`
	Master   string `json:"master"`
	LinkInfo struct {
		InfoKind      string `json:"info_kind"`
		InfoSlaveKind string `json:"info_slave_kind"`
		InfoData      struct {
			Mode string `json:"mode"`
		} `json:"info_data"`
		InfoSlaveData struct {
			State     string `json:"state"`
			MIIStatus string `json:"mii_status"`
			ActorOper int    `json:"ad_actor_oper_port_state"`
		} `json:"info_slave_data"`
	} `json:"linkinfo"`
}

func linkDetails() ([]ipLinkDetail, error) {
	out, err := exec.Command("ip", "-j", "-d", "link", "show").Output()
	if err != nil {
		return nil, fmt.Errorf("ip -j -d link show: %w", err)
	}
	var l []ipLinkDetail
	if err := json.Unmarshal(out, &l); err != nil {
		return nil, fmt.Errorf("ip -j -d link show: %w", err)
	}
	return l, nil
}

func validLAGName(name string) error {
	var n int
	if _, err := fmt.Sscanf(name, "po%d", &n); err != nil || n < 1 || switchapi.LAGName(n) != name {
		return fmt.Errorf("%q is not a LAG name: po1, po2 ...", name)
	}
	return nil
}

// isLAG is whether name is a bond this driver made.
func isLAG(name string) bool {
	if validLAGName(name) != nil {
		return false
	}
	l, err := linkDetails()
	if err != nil {
		return false
	}
	for _, d := range l {
		if d.IfName == name {
			return d.LinkInfo.InfoKind == "bond"
		}
	}
	return false
}

func bondMode(lacp bool) string {
	if lacp {
		return "802.3ad"
	}
	return "balance-xor"
}

func (s *Switch) AddLAG(name string, lacp bool) error {
	if !s.lags {
		return switchapi.Unsupported("link aggregation")
	}
	if err := validLAGName(name); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if !exists(name) {
		args := []string{"link", "add", name, "type", "bond", "mode", bondMode(lacp),
			"miimon", "100", "xmit_hash_policy", "layer3+4"}
		if lacp {
			args = append(args, "lacp_rate", "fast")
		}
		if _, err := ipCmd(args...); err != nil {
			return err
		}
		_, err := ipCmd("link", "set", name, "up")
		return err
	}
	// A bond's mode changes only with no members and the bond down, so a mode
	// change is: release them, change it, take them back.
	members, err := s.lagMembers(name)
	if err != nil {
		return err
	}
	for _, m := range members {
		if _, err := ipCmd("link", "set", m, "nomaster"); err != nil {
			return err
		}
	}
	if _, err := ipCmd("link", "set", name, "down"); err != nil {
		return err
	}
	args := []string{"link", "set", name, "type", "bond", "mode", bondMode(lacp)}
	if lacp {
		args = append(args, "lacp_rate", "fast")
	}
	if _, err := ipCmd(args...); err != nil {
		return err
	}
	if _, err := ipCmd("link", "set", name, "up"); err != nil {
		return err
	}
	for _, m := range members {
		if err := enslave(m, name); err != nil {
			return err
		}
	}
	return nil
}

// release takes a port out of its bond and brings it back up. Leaving a bond
// leaves the port administratively DOWN, and a port freed from a LAG has to be
// an ordinary, working port again -- the conformance suite caught the first
// version routing nothing through a port it had just released.
func release(port string) error {
	if _, err := ipCmd("link", "set", port, "nomaster"); err != nil {
		return err
	}
	_, err := ipCmd("link", "set", port, "up")
	return err
}

func enslave(port, bond string) error {
	// A bond takes a member only while the member is down.
	if _, err := ipCmd("link", "set", port, "down"); err != nil {
		return err
	}
	if _, err := ipCmd("link", "set", port, "master", bond); err != nil {
		return err
	}
	_, err := ipCmd("link", "set", port, "up")
	return err
}

// lagMembers is the ports whose master is the bond. Callers hold s.mu.
func (s *Switch) lagMembers(name string) ([]string, error) {
	l, err := linkDetails()
	if err != nil {
		return nil, err
	}
	var out []string
	for _, d := range l {
		if d.Master == name {
			out = append(out, d.IfName)
		}
	}
	sort.Strings(out)
	return out, nil
}

func (s *Switch) SetLAGMembers(name string, ports []string) error {
	if !s.lags {
		return switchapi.Unsupported("link aggregation")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if !exists(name) {
		return fmt.Errorf("%s does not exist", name)
	}
	l, err := linkDetails()
	if err != nil {
		return err
	}
	master := map[string]string{}
	for _, d := range l {
		master[d.IfName] = d.Master
	}
	want := map[string]bool{}
	for _, p := range ports {
		if err := s.known(p); err != nil {
			return err
		}
		switch m := master[p]; {
		case m == bridgeName:
			return fmt.Errorf("%s is a switched port; take it out of its VLANs first", p)
		case m != "" && m != name:
			return fmt.Errorf("%s is already a member of %s", p, m)
		}
		want[p] = true
	}
	for p, m := range master {
		if m == name && !want[p] {
			if err := release(p); err != nil {
				return err
			}
		}
	}
	for _, p := range ports {
		if master[p] != name {
			if err := enslave(p, name); err != nil {
				return err
			}
		}
	}
	return nil
}

func (s *Switch) DelLAG(name string) error {
	if !s.lags {
		return switchapi.Unsupported("link aggregation")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if !exists(name) {
		return nil
	}
	members, err := s.lagMembers(name)
	if err != nil {
		return err
	}
	for _, m := range members {
		if err := release(m); err != nil {
			return err
		}
	}
	_, err = ipCmd("link", "del", name)
	return err
}

func (s *Switch) LAGs() ([]switchapi.LAG, error) {
	if !s.lags {
		return nil, switchapi.Unsupported("link aggregation")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	l, err := linkDetails()
	if err != nil {
		return nil, err
	}
	var out []switchapi.LAG
	for _, d := range l {
		if d.LinkInfo.InfoKind != "bond" || validLAGName(d.IfName) != nil {
			continue
		}
		g := switchapi.LAG{Name: d.IfName, LACP: d.LinkInfo.InfoData.Mode == "802.3ad"}
		for _, m := range l {
			if m.Master != d.IfName {
				continue
			}
			sd := m.LinkInfo.InfoSlaveData
			active := sd.State == "ACTIVE" && sd.MIIStatus == "UP"
			if g.LACP {
				// Collecting (0x10) and distributing (0x20) in the actor's
				// own state: LACP has agreed and traffic flows both ways.
				active = active && sd.ActorOper&0x30 == 0x30
			}
			g.Members = append(g.Members, switchapi.LAGMember{Port: m.IfName, Active: active})
		}
		sort.Slice(g.Members, func(i, j int) bool { return g.Members[i].Port < g.Members[j].Port })
		out = append(out, g)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

// lagMemberOf is the bond a port belongs to, or "".
func lagMemberOf(port string) string {
	l, err := linkDetails()
	if err != nil {
		return ""
	}
	for _, d := range l {
		if d.IfName == port && d.LinkInfo.InfoSlaveKind == "bond" {
			return d.Master
		}
	}
	return ""
}
