package virt

import (
	"encoding/json"
	"fmt"
	"os/exec"
	"sort"
	"strconv"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// VLANs on the virtual board are a Linux bridge with VLAN filtering, which is
// the kernel's own model of an 802.1Q switch: a port joins it, and per-port
// VLAN membership says which VLANs it carries and which one is native.
//
// The bridge stands in for the chip, and it is where the state lives. Nothing
// here is remembered in the process, so a restarted nosd reads back what the
// kernel holds rather than forgetting it -- the same rule Start follows for
// the ports themselves.
//
//   - A VLAN exists when the bridge itself is a member of it ("self"). That is
//     the CPU port on a real chip: the switch's own interface into the VLAN.
//   - A port with any membership is enslaved to the bridge; one with none is
//     released and routes again, as the contract says.
//   - An SVI is a vlan device on top of the bridge, vlan<VID>, which is how a
//     routed VLAN interface looks to the kernel on every Linux switch.
//
// The bridge is created with vlan_default_pvid 0. The kernel's default puts
// every new member in VLAN 1 untagged, which is a VLAN nobody configured and a
// membership VLANs() would then report.
const bridgeName = "br0"

// bridgeWorks reports whether the bridge tool is here to drive, which decides
// whether VLANs are real or refused -- the same honesty rule as nftables.
func bridgeWorks() bool {
	_, err := exec.LookPath("bridge")
	return err == nil
}

func bridgeCmd(args ...string) error {
	out, err := exec.Command("bridge", args...).CombinedOutput()
	if err != nil {
		return fmt.Errorf("bridge %s: %w: %s",
			strings.Join(args, " "), err, strings.TrimSpace(string(out)))
	}
	return nil
}

func (s *Switch) ensureBridge() error {
	if exists(bridgeName) {
		return nil
	}
	if _, err := ipCmd("link", "add", bridgeName, "type", "bridge",
		"vlan_filtering", "1", "vlan_default_pvid", "0"); err != nil {
		return err
	}
	// A MAC of its own, set rather than inherited. A bridge left without
	// ports falls back to all zeros, and an SVI made on it then refuses to
	// come up with "Cannot assign requested address" -- the conformance
	// suite hit exactly that, after it had released its last port. A chip
	// has a router MAC that does not depend on which ports are in a VLAN,
	// and this is that.
	if _, err := ipCmd("link", "set", bridgeName, "address", routerMAC); err != nil {
		return err
	}
	_, err := ipCmd("link", "set", bridgeName, "up")
	return err
}

// routerMAC is the virtual board's router MAC: locally administered, so it
// cannot collide with a real vendor's address.
const routerMAC = "02:00:5e:00:00:01"

// bridgeVLANs is `bridge -j vlan show`: VID -> membership, per interface.
type vlanFlags struct {
	Tagged bool
	PVID   bool
}

func bridgeVLANs() (map[string]map[int]vlanFlags, error) {
	out, err := exec.Command("bridge", "-j", "vlan", "show").Output()
	if err != nil {
		return nil, fmt.Errorf("bridge vlan show: %w", err)
	}
	var raw []struct {
		IfName string `json:"ifname"`
		VLANs  []struct {
			VLAN    int      `json:"vlan"`
			VLANEnd int      `json:"vlanEnd"`
			Flags   []string `json:"flags"`
		} `json:"vlans"`
	}
	if len(strings.TrimSpace(string(out))) > 0 {
		if err := json.Unmarshal(out, &raw); err != nil {
			return nil, fmt.Errorf("bridge vlan show: %w", err)
		}
	}
	m := map[string]map[int]vlanFlags{}
	for _, r := range raw {
		for _, v := range r.VLANs {
			f := vlanFlags{Tagged: true}
			for _, fl := range v.Flags {
				switch fl {
				case "Egress Untagged":
					f.Tagged = false
				case "PVID":
					f.PVID = true
				}
			}
			end := v.VLANEnd
			if end < v.VLAN {
				end = v.VLAN
			}
			for vid := v.VLAN; vid <= end; vid++ {
				if m[r.IfName] == nil {
					m[r.IfName] = map[int]vlanFlags{}
				}
				m[r.IfName][vid] = f
			}
		}
	}
	return m, nil
}

func (s *Switch) vlanExists(vid int) (bool, error) {
	if !exists(bridgeName) {
		return false, nil
	}
	m, err := bridgeVLANs()
	if err != nil {
		return false, err
	}
	_, ok := m[bridgeName][vid]
	return ok, nil
}

func validVID(vid int) error {
	if vid < 1 || vid > 4094 {
		return fmt.Errorf("vlan %d out of range 1-4094", vid)
	}
	return nil
}

func (s *Switch) AddVLAN(vid int) error {
	if !s.vlans {
		return switchapi.Unsupported("vlans")
	}
	if err := validVID(vid); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.ensureBridge(); err != nil {
		return err
	}
	// Tagged on the bridge itself: that is the path an SVI sends through.
	return bridgeCmd("vlan", "add", "vid", strconv.Itoa(vid), "dev", bridgeName, "self")
}

func (s *Switch) DelVLAN(vid int) error {
	if !s.vlans {
		return switchapi.Unsupported("vlans")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if exists(switchapi.SVIName(vid)) {
		return fmt.Errorf("vlan %d has a routed interface; remove %s first", vid, switchapi.SVIName(vid))
	}
	m, err := bridgeVLANs()
	if err != nil {
		return err
	}
	for _, name := range s.switchable(m) {
		if _, ok := m[name][vid]; ok {
			if err := s.leave(name, vid, m); err != nil {
				return err
			}
		}
	}
	if _, ok := m[bridgeName][vid]; ok {
		return bridgeCmd("vlan", "del", "vid", strconv.Itoa(vid), "dev", bridgeName, "self")
	}
	return nil
}

func (s *Switch) SetPortVLAN(name string, vid int, tagged bool) error {
	if !s.vlans {
		return switchapi.Unsupported("vlans")
	}
	if err := s.known(name); err != nil {
		return err
	}
	if m := lagMemberOf(name); m != "" {
		return fmt.Errorf("%s is a member of %s; put %s in the VLAN instead", name, m, m)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	ok, err := s.vlanExists(vid)
	if err != nil {
		return err
	}
	if !ok {
		return fmt.Errorf("vlan %d does not exist", vid)
	}
	if _, err := ipCmd("link", "set", name, "master", bridgeName); err != nil {
		return err
	}
	m, err := bridgeVLANs()
	if err != nil {
		return err
	}
	args := []string{"vlan", "add", "vid", strconv.Itoa(vid), "dev", name}
	if !tagged {
		// One native VLAN per port: drop the old untagged membership first.
		for v, f := range m[name] {
			if !f.Tagged && v != vid {
				if err := bridgeCmd("vlan", "del", "vid", strconv.Itoa(v), "dev", name); err != nil {
					return err
				}
			}
		}
		args = append(args, "pvid", "untagged")
	} else if f, ok := m[name][vid]; ok && !f.Tagged {
		// Re-adding does not clear flags it is not given, so a native VLAN
		// turned tagged has to be removed and added back.
		if err := bridgeCmd("vlan", "del", "vid", strconv.Itoa(vid), "dev", name); err != nil {
			return err
		}
	}
	return bridgeCmd(args...)
}

func (s *Switch) DelPortVLAN(name string, vid int) error {
	if !s.vlans {
		return switchapi.Unsupported("vlans")
	}
	if err := s.known(name); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	m, err := bridgeVLANs()
	if err != nil {
		return err
	}
	if _, ok := m[name][vid]; !ok {
		return nil
	}
	return s.leave(name, vid, m)
}

// leave removes one membership and releases the port from the bridge when it
// was the last, so it routes again.
func (s *Switch) leave(name string, vid int, m map[string]map[int]vlanFlags) error {
	if err := bridgeCmd("vlan", "del", "vid", strconv.Itoa(vid), "dev", name); err != nil {
		return err
	}
	delete(m[name], vid)
	if len(m[name]) == 0 {
		_, err := ipCmd("link", "set", name, "nomaster")
		return err
	}
	return nil
}

func (s *Switch) VLANs() ([]switchapi.VLAN, error) {
	if !s.vlans {
		return nil, switchapi.Unsupported("vlans")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if !exists(bridgeName) {
		return nil, nil
	}
	m, err := bridgeVLANs()
	if err != nil {
		return nil, err
	}
	var out []switchapi.VLAN
	for vid := range m[bridgeName] {
		v := switchapi.VLAN{VID: vid, SVI: exists(switchapi.SVIName(vid))}
		for _, name := range s.switchable(m) {
			if f, ok := m[name][vid]; ok {
				v.Members = append(v.Members, switchapi.VLANMember{Port: name, Tagged: f.Tagged})
			}
		}
		out = append(out, v)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].VID < out[j].VID })
	return out, nil
}

func (s *Switch) AddSVI(vid int) error {
	if !s.vlans {
		return switchapi.Unsupported("routed vlan interfaces")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	ok, err := s.vlanExists(vid)
	if err != nil {
		return err
	}
	if !ok {
		return fmt.Errorf("vlan %d does not exist", vid)
	}
	name := switchapi.SVIName(vid)
	if !exists(name) {
		if _, err := ipCmd("link", "add", "link", bridgeName, "name", name,
			"type", "vlan", "id", strconv.Itoa(vid)); err != nil {
			return err
		}
	}
	_, err = ipCmd("link", "set", name, "up")
	return err
}

func (s *Switch) DelSVI(vid int) error {
	if !s.vlans {
		return switchapi.Unsupported("routed vlan interfaces")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	name := switchapi.SVIName(vid)
	if !exists(name) {
		return nil
	}
	_, err := ipCmd("link", "del", name)
	return err
}

// knownL3 accepts anything that takes an address: a port, or an SVI this
// datapath made.
func (s *Switch) knownL3(name string) error {
	if s.known(name) == nil {
		return nil
	}
	if strings.HasPrefix(name, "vlan") && s.vlans && exists(name) {
		if _, err := strconv.Atoi(strings.TrimPrefix(name, "vlan")); err == nil {
			return nil
		}
	}
	return fmt.Errorf("no such port or vlan interface %q", name)
}

// switchable is everything that can be in a VLAN: the ports, then any LAG the
// bridge holds a membership for.
func (s *Switch) switchable(m map[string]map[int]vlanFlags) []string {
	out := append([]string(nil), s.names...)
	var lags []string
	for name := range m {
		if validLAGName(name) == nil {
			lags = append(lags, name)
		}
	}
	sort.Strings(lags)
	return append(out, lags...)
}
