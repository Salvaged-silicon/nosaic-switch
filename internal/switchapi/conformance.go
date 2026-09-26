package switchapi

import (
	"errors"
	"fmt"
	"net/netip"
	"strings"
	"testing"
)

// Check runs the contract's conformance suite and returns every problem found.
//
// It is a plain function rather than a test helper for two reasons. It can be
// tested itself — a suite that passes everything proves nothing, so there is a
// test that hands it a deliberately dishonest implementation and requires it
// to complain. And it can be run against real hardware from the CLI, so the
// same checks that gate the virtual board can be pointed at a switch.
//
// Every datapath must pass this unchanged. If a future implementation needs it
// relaxed, the abstraction has failed and the fix belongs in the contract.
func Check(sw Switch) []error {
	var probs []error
	bad := func(f string, a ...any) { probs = append(probs, fmt.Errorf(f, a...)) }

	caps := sw.Capabilities()
	if caps.Driver == "" {
		bad("Capabilities().Driver is empty: an implementation must name itself")
	}
	if caps.Contract == "" {
		bad("Capabilities().Contract is empty: an implementation must say which contract version it targets")
	}

	if err := sw.Start(); err != nil {
		return append(probs, fmt.Errorf("Start: %w", err))
	}
	defer sw.Close()

	ports, err := sw.Ports()
	if err != nil {
		return append(probs, fmt.Errorf("Ports: %w", err))
	}
	if len(ports) == 0 {
		return append(probs, errors.New("Ports returned nothing: a datapath with no ports cannot be configured"))
	}
	if caps.MaxPorts > 0 && len(ports) > caps.MaxPorts {
		bad("Ports returned %d ports, more than the declared maximum of %d", len(ports), caps.MaxPorts)
	}
	p0 := ports[0].Name

	// Admin state must round-trip: what was asked for must be readable back.
	if err := sw.SetPortAdmin(p0, true); err != nil {
		bad("SetPortAdmin(up): %v", err)
	} else if st, err := sw.PortStatus(p0); err != nil {
		bad("PortStatus: %v", err)
	} else if !st.AdminUp {
		bad("%s was set admin-up but does not report it", p0)
	}
	if err := sw.SetPortAdmin(p0, false); err == nil {
		if st, _ := sw.PortStatus(p0); st.AdminUp {
			bad("%s was set admin-down but still reports admin-up", p0)
		}
	}
	_ = sw.SetPortAdmin(p0, true)

	if _, err := sw.PortStatus("swp-nonexistent"); err == nil {
		bad("PortStatus on an unknown port succeeded; it must fail rather than return a zero value")
	}

	probs = append(probs, checkVLANs(sw, caps, p0)...)
	if len(ports) >= 2 {
		probs = append(probs, checkLAGs(sw, caps, p0, ports[1].Name)...)
		probs = append(probs, checkMLAG(sw, caps, p0, ports[1].Name)...)
	}
	probs = append(probs, checkSTP(sw, caps, p0)...)
	probs = append(probs, checkL3(sw, caps, p0)...)

	probs = append(probs, checkACLs(sw, caps, p0)...)

	_, err = sw.PortCounters(p0)
	probs = append(probs, wantSupport(caps.Counters, err, "PortCounters", "Capabilities.Counters")...)

	_, err = sw.FDB()
	probs = append(probs, wantSupport(caps.L2Learning, err, "FDB", "Capabilities.L2Learning")...)

	return probs
}

func checkVLANs(sw Switch, caps Capabilities, p0 string) []error {
	err := sw.AddVLAN(100)
	if probs := wantSupport(caps.VLANs, err, "AddVLAN", "Capabilities.VLANs"); len(probs) > 0 || !caps.VLANs {
		return probs
	}
	var probs []error
	bad := func(f string, a ...any) { probs = append(probs, fmt.Errorf(f, a...)) }
	defer sw.DelVLAN(100)
	if err := sw.AddVLAN(200); err != nil {
		return append(probs, fmt.Errorf("AddVLAN(200): %w", err))
	}
	defer sw.DelVLAN(200)

	// What was configured must be what is listed. A datapath that accepts
	// a membership and cannot say so afterwards cannot be audited.
	member := func(vid int) (VLANMember, bool) {
		vl, err := sw.VLANs()
		if err != nil {
			bad("VLANs: %v", err)
			return VLANMember{}, false
		}
		for _, v := range vl {
			if v.VID != vid {
				continue
			}
			for _, m := range v.Members {
				if m.Port == p0 {
					return m, true
				}
			}
		}
		return VLANMember{}, false
	}

	// One native VLAN per port: a second untagged membership replaces the
	// first, because an untagged frame can only go into one VLAN.
	if err := sw.SetPortVLAN(p0, 100, false); err != nil {
		bad("SetPortVLAN(%s, 100, untagged): %v", p0, err)
	}
	if err := sw.SetPortVLAN(p0, 200, false); err != nil {
		bad("SetPortVLAN(%s, 200, untagged): %v", p0, err)
	}
	if m, ok := member(200); !ok || m.Tagged {
		bad("%s was made an untagged member of 200 and VLANs does not list it so", p0)
	}
	if _, ok := member(100); ok {
		bad("%s is still in vlan 100 after its native vlan moved to 200: "+
			"a port has one native vlan", p0)
	}

	// Tagged alongside the native VLAN is a trunk.
	if err := sw.SetPortVLAN(p0, 100, true); err != nil {
		bad("SetPortVLAN(%s, 100, tagged): %v", p0, err)
	}
	if m, ok := member(100); !ok || !m.Tagged {
		bad("%s was made a tagged member of 100 and VLANs does not list it so", p0)
	}
	if _, ok := member(200); !ok {
		bad("adding a tagged vlan removed %s's native vlan", p0)
	}

	if err := sw.DelPortVLAN(p0, 100); err != nil {
		bad("DelPortVLAN: %v", err)
	} else if _, ok := member(100); ok {
		bad("%s was removed from vlan 100 and VLANs still lists it", p0)
	}
	if err := sw.DelPortVLAN(p0, 200); err != nil {
		bad("DelPortVLAN: %v", err)
	}

	probs = append(probs, checkSVIs(sw, caps)...)
	return probs
}

func checkSVIs(sw Switch, caps Capabilities) []error {
	err := sw.AddSVI(200)
	if probs := wantSupport(caps.SVIs, err, "AddSVI", "Capabilities.SVIs"); len(probs) > 0 || !caps.SVIs {
		return probs
	}
	var probs []error
	bad := func(f string, a ...any) { probs = append(probs, fmt.Errorf(f, a...)) }
	name := SVIName(200)

	if vl, err := sw.VLANs(); err != nil {
		bad("VLANs: %v", err)
	} else {
		found := false
		for _, v := range vl {
			found = found || (v.VID == 200 && v.SVI)
		}
		if !found {
			bad("AddSVI(200) succeeded and VLANs does not report an SVI on 200")
		}
	}
	// An SVI is an L3 interface: it takes an address like a port does.
	if caps.L3 {
		addr := netip.MustParsePrefix("10.99.200.1/24")
		if err := sw.AddAddress(name, addr); err != nil {
			bad("AddAddress(%s): %v", name, err)
		} else if err := sw.DelAddress(name, addr); err != nil {
			bad("DelAddress(%s): %v", name, err)
		}
	}
	// Deleting a VLAN out from under its routed interface would leave an
	// interface with addresses and nothing to route for.
	if err := sw.DelVLAN(200); err == nil {
		bad("DelVLAN(200) succeeded while %s existed; it must be refused", name)
		_ = sw.AddVLAN(200)
	}
	if err := sw.DelSVI(200); err != nil {
		bad("DelSVI: %v", err)
	}
	if err := sw.AddSVI(4000); err == nil {
		bad("AddSVI on a vlan that does not exist succeeded")
		_ = sw.DelSVI(4000)
	}
	return probs
}

func checkLAGs(sw Switch, caps Capabilities, p0, p1 string) []error {
	err := sw.AddLAG("po1", false)
	if probs := wantSupport(caps.LAGs, err, "AddLAG", "Capabilities.LAGs"); len(probs) > 0 || !caps.LAGs {
		return probs
	}
	var probs []error
	bad := func(f string, a ...any) { probs = append(probs, fmt.Errorf(f, a...)) }
	defer sw.DelLAG("po1")

	members := func() (LAG, bool) {
		ls, err := sw.LAGs()
		if err != nil {
			bad("LAGs: %v", err)
			return LAG{}, false
		}
		for _, l := range ls {
			if l.Name == "po1" {
				return l, true
			}
		}
		return LAG{}, false
	}
	has := func(l LAG, port string) bool {
		for _, m := range l.Members {
			if m.Port == port {
				return true
			}
		}
		return false
	}

	if err := sw.AddLAG("bogus", false); err == nil {
		bad("AddLAG accepted %q, which is not a po<N> name", "bogus")
		_ = sw.DelLAG("bogus")
	}
	if err := sw.SetLAGMembers("po1", []string{p0, p1}); err != nil {
		return append(probs, fmt.Errorf("SetLAGMembers(po1, %s %s): %w", p0, p1, err))
	}
	if l, ok := members(); !ok || !has(l, p0) || !has(l, p1) {
		bad("po1 was given %s and %s and LAGs does not list both", p0, p1)
	}
	// A member is the LAG's, not its own.
	if caps.VLANs {
		if err := sw.AddVLAN(300); err == nil {
			if err := sw.SetPortVLAN(p0, 300, false); err == nil {
				bad("%s is a member of po1 and still took a VLAN of its own", p0)
				_ = sw.DelPortVLAN(p0, 300)
			}
			// The LAG itself is a switchport like any other.
			if err := sw.SetPortVLAN("po1", 300, false); err != nil {
				bad("SetPortVLAN(po1): a LAG must take a VLAN like a port: %v", err)
			} else if vl, err := sw.VLANs(); err == nil {
				found := false
				for _, v := range vl {
					for _, m := range v.Members {
						found = found || (v.VID == 300 && m.Port == "po1")
					}
				}
				if !found {
					bad("po1 was put in vlan 300 and VLANs does not list it")
				}
				_ = sw.DelPortVLAN("po1", 300)
			}
			_ = sw.DelVLAN(300)
		}
	}
	// ... and a routed port like any other.
	if caps.L3 {
		addr := netip.MustParsePrefix("10.99.1.1/24")
		if err := sw.AddAddress("po1", addr); err != nil {
			bad("AddAddress(po1): a LAG must take an address like a port: %v", err)
		} else {
			_ = sw.DelAddress("po1", addr)
		}
	}
	// End state: naming only p1 takes p0 out.
	if err := sw.SetLAGMembers("po1", []string{p1}); err != nil {
		bad("SetLAGMembers(po1, %s): %v", p1, err)
	} else if l, ok := members(); !ok || has(l, p0) || !has(l, p1) {
		bad("po1 was restated as just %s and LAGs does not show exactly that", p1)
	}
	if caps.LACP {
		if err := sw.AddLAG("po1", true); err != nil {
			bad("AddLAG(po1, lacp): %v", err)
		} else if l, _ := members(); !l.LACP {
			bad("po1 was made LACP and LAGs does not say so")
		} else if !has(l, p1) {
			bad("changing po1's mode lost its members")
		}
	}
	if err := sw.DelLAG("po1"); err != nil {
		bad("DelLAG: %v", err)
	} else if _, ok := members(); ok {
		bad("po1 was deleted and LAGs still lists it")
	}
	return probs
}

func checkL3(sw Switch, caps Capabilities, p0 string) []error {
	addr := netip.MustParsePrefix("10.99.0.1/24")
	err := sw.AddAddress(p0, addr)
	if probs := wantSupport(caps.L3, err, "AddAddress", "Capabilities.L3"); len(probs) > 0 || !caps.L3 {
		return probs
	}
	var probs []error
	bad := func(f string, a ...any) { probs = append(probs, fmt.Errorf(f, a...)) }
	defer sw.DelAddress(p0, addr)

	single := Route{
		Prefix:   netip.MustParsePrefix("10.100.0.0/24"),
		NextHops: []NextHop{{Via: netip.MustParseAddr("10.99.0.2"), Port: p0}},
	}
	if err := sw.AddRoute(single); err != nil {
		bad("AddRoute with a single next-hop: %v", err)
	} else {
		if routes, err := sw.Routes(); err != nil {
			bad("Routes: %v", err)
		} else if !hasRoute(routes, single.Prefix) {
			bad("route %s was added but is not listed", single.Prefix)
		}
		if err := sw.DelRoute(single.Prefix); err != nil {
			bad("DelRoute: %v", err)
		}
	}

	// The heart of the contract. An implementation without multipath must
	// refuse a multipath route rather than install one path and report
	// success — a switch quietly forwarding over half the paths you asked for
	// is worse than one that refuses, because nothing tells you.
	multi := Route{
		Prefix: netip.MustParsePrefix("10.101.0.0/24"),
		NextHops: []NextHop{
			{Via: netip.MustParseAddr("10.99.0.2"), Port: p0},
			{Via: netip.MustParseAddr("10.99.0.3"), Port: p0},
		},
	}
	err = sw.AddRoute(multi)
	if caps.SupportsECMP(2) {
		if err != nil {
			bad("Capabilities claims ECMP, but a two-path route failed: %v", err)
		} else {
			_ = sw.DelRoute(multi.Prefix)
		}
		return probs
	}
	return append(probs, wantSupport(false, err,
		"AddRoute with two next-hops",
		"Capabilities says ECMP is unavailable")...)
}

func checkACLs(sw Switch, caps Capabilities, p0 string) []error {
	v4, _ := ParseACLRule(10, "deny in "+p0+" proto icmp src 192.0.2.0/24")
	err := sw.SetACL(v4)
	if probs := wantSupport(caps.ACL, err, "SetACL", "Capabilities.ACL"); len(probs) > 0 || !caps.ACL {
		return probs
	}
	var probs []error
	bad := func(f string, a ...any) { probs = append(probs, fmt.Errorf(f, a...)) }
	defer sw.DelACL(10)

	// What was set must be listed, installed, as the same rule.
	entries, err := sw.ACLs()
	if err != nil {
		bad("ACLs: %v", err)
	} else if e, ok := findACL(entries, 10); !ok {
		bad("rule 10 was set but is not listed")
	} else {
		if !e.Installed {
			bad("rule 10 was accepted but is listed as not installed: %s", e.Error)
		}
		if e.Parsed && e.Rule.String() != v4.String() {
			bad("rule 10 came back as %q, was set as %q", e.Rule.String(), v4.String())
		}
	}

	// Setting the same sequence again replaces, never duplicates.
	again, _ := ParseACLRule(10, "permit in "+p0+" proto icmp src 192.0.2.0/24")
	if err := sw.SetACL(again); err != nil {
		bad("SetACL replacing a rule: %v", err)
	} else if entries, err := sw.ACLs(); err == nil {
		n := 0
		for _, e := range entries {
			if e.Seq == 10 {
				n++
				if e.Parsed && e.Rule.Action != ACLPermit {
					bad("rule 10 was replaced but still reads as the old rule")
				}
			}
		}
		if n != 1 {
			bad("setting sequence 10 twice left %d rules with that sequence", n)
		}
	}

	// The refusals every implementation owes, with an ordinary error, so a
	// caller can tell "you asked for something wrong" from "cannot".
	noPort, _ := ParseACLRule(11, "deny in swp-nonexistent proto icmp")
	if err := sw.SetACL(noPort); err == nil {
		bad("SetACL accepted a rule on a port the switch does not have")
		_ = sw.DelACL(11)
	} else if errors.Is(err, ErrUnsupported) {
		bad("SetACL refused an unknown port with ErrUnsupported; that is a bad rule, not a missing capability")
	}
	noL4 := ACLRule{Seq: 12, Family: 4, Action: ACLDeny, Proto: ACLAny, SrcPort: ACLAny, DstPort: 22}
	if err := sw.SetACL(noL4); err == nil {
		bad("SetACL accepted an L4 port match without TCP or UDP")
		_ = sw.DelACL(12)
	}

	// Deleting removes it; deleting again is an error, not a no-op.
	if err := sw.DelACL(10); err != nil {
		bad("DelACL: %v", err)
	} else if entries, _ := sw.ACLs(); hasACL(entries, 10) {
		bad("rule 10 was deleted but is still listed")
	}
	if err := sw.DelACL(10); err == nil {
		bad("DelACL of a rule that does not exist succeeded")
	}

	// IPv6 is its own capability, because it is its own field group on
	// every chip so far.
	v6, _ := ParseACLRule(13, "deny in "+p0+" ipv6 proto icmpv6 src 2001:db8::/32")
	err = sw.SetACL(v6)
	probs = append(probs, wantSupport(caps.ACL6, err, "SetACL with an IPv6 rule", "Capabilities.ACL6")...)
	if err == nil {
		if entries, _ := sw.ACLs(); !hasACL(entries, 13) {
			bad("IPv6 rule 13 was set but is not listed")
		}
		_ = sw.DelACL(13)
	}
	return probs
}

func findACL(entries []ACLEntry, seq int) (ACLEntry, bool) {
	for _, e := range entries {
		if e.Seq == seq {
			return e, true
		}
	}
	return ACLEntry{}, false
}

func hasACL(entries []ACLEntry, seq int) bool {
	_, ok := findACL(entries, seq)
	return ok
}

// wantSupport reconciles a declared capability with what actually happened.
//
// It is the check that stops the capability model being decoration: an
// implementation that says it cannot do something must refuse, and one that
// says it can must not fail with ErrUnsupported.
func wantSupport(declared bool, err error, op, capName string) []error {
	switch {
	case declared && errors.Is(err, ErrUnsupported):
		return []error{fmt.Errorf("%s returned ErrUnsupported although %s is true", op, capName)}
	case declared:
		if err != nil {
			return []error{fmt.Errorf("%s: %w", op, err)}
		}
		return nil
	case err == nil:
		return []error{fmt.Errorf(
			"%s succeeded although %s is false: capabilities and behaviour must agree, "+
				"or the capability model is decoration", op, capName)}
	case !errors.Is(err, ErrUnsupported):
		return []error{fmt.Errorf(
			"%s failed with %v, which does not wrap ErrUnsupported: callers cannot tell "+
				"'this hardware cannot' from 'this went wrong'", op, err)}
	}
	return nil
}

// Conform runs Check and reports each problem as a test failure.
func Conform(t *testing.T, sw Switch) {
	t.Helper()
	for _, p := range Check(sw) {
		t.Error(p)
	}
}

func hasRoute(routes []Route, p netip.Prefix) bool {
	for _, r := range routes {
		if r.Prefix == p {
			return true
		}
	}
	return false
}

// checkSTP is what one bridge can check about its own spanning tree without
// knowing who its neighbours are: configuration reads back, nonsense is
// refused, a switched interface appears with its cost, and "no root port" means
// "I am the root". It leaves spanning tree off, as it found a fresh datapath.
func checkSTP(sw Switch, caps Capabilities, p0 string) []error {
	err := sw.SetSTP(STPConfig{Enabled: false, Priority: STPDefaultPriority})
	if probs := wantSupport(caps.STP, err, "SetSTP", "Capabilities.STP"); len(probs) > 0 || !caps.STP {
		return probs
	}
	var probs []error
	bad := func(f string, a ...any) { probs = append(probs, fmt.Errorf(f, a...)) }
	defer func() {
		_ = sw.SetSTP(STPConfig{Enabled: false, Priority: STPDefaultPriority})
		_ = sw.SetSTPPort(p0, STPPortConfig{})
	}()

	if err := sw.SetSTP(STPConfig{Enabled: true, Priority: 1000}); err == nil {
		bad("SetSTP accepted priority 1000, which is not a multiple of 4096")
	}
	if err := sw.SetSTPPort("swp-nonexistent", STPPortConfig{}); err == nil {
		bad("SetSTPPort on an unknown port succeeded")
	}
	if err := sw.SetSTPPort(p0, STPPortConfig{Cost: -1}); err == nil {
		bad("SetSTPPort accepted cost -1")
	}
	if err := sw.SetSTPPort(p0, STPPortConfig{Cost: 1234}); err != nil {
		bad("SetSTPPort(%s, cost 1234): %v", p0, err)
	}
	if err := sw.SetSTP(STPConfig{Enabled: true, Priority: 4096}); err != nil {
		return append(probs, fmt.Errorf("SetSTP(on, 4096): %w", err))
	}

	// A switched interface is in the tree.
	if caps.VLANs {
		if err := sw.AddVLAN(301); err == nil {
			defer sw.DelVLAN(301)
			if err := sw.SetPortVLAN(p0, 301, true); err == nil {
				defer sw.DelPortVLAN(p0, 301)
			} else {
				bad("SetPortVLAN(%s, 301): %v", p0, err)
			}
		}
	}
	st, err := sw.STP()
	if err != nil {
		return append(probs, fmt.Errorf("STP: %w", err))
	}
	if !st.Enabled || st.Priority != 4096 {
		bad("STP reports enabled=%v priority=%d after SetSTP(on, 4096)", st.Enabled, st.Priority)
	}
	if !strings.HasPrefix(st.BridgeID, "1000.") {
		bad("bridge ID %q does not carry priority 4096 as 1000.<mac>", st.BridgeID)
	}
	if (st.RootPort == "") != (st.RootID == st.BridgeID) {
		bad("root port %q and root %s disagree about whether this bridge (%s) is the root",
			st.RootPort, st.RootID, st.BridgeID)
	}
	if caps.VLANs {
		var found bool
		for _, p := range st.Ports {
			if p.Port != p0 {
				continue
			}
			found = true
			if p.Cost != 1234 {
				bad("%s reports path cost %d, configured 1234", p0, p.Cost)
			}
			switch p.Role {
			case "root", "designated", "alternate", "backup", "disabled":
			default:
				bad("%s has role %q", p0, p.Role)
			}
			switch p.State {
			case "discarding", "learning", "forwarding":
			default:
				bad("%s has state %q", p0, p.State)
			}
		}
		if !found {
			bad("%s is in VLAN 301 but not in the spanning tree", p0)
		}
	}

	if err := sw.SetSTP(STPConfig{Enabled: false, Priority: STPDefaultPriority}); err != nil {
		bad("SetSTP(off): %v", err)
	} else if st, err := sw.STP(); err == nil {
		if st.Enabled {
			bad("STP still enabled after SetSTP(off)")
		}
		for _, p := range st.Ports {
			if p.State != "forwarding" {
				bad("spanning tree is off but %s is %s", p.Port, p.State)
			}
		}
	}
	return probs
}

// checkMLAG is what one switch can check about MLAG with no peer: the
// configuration is refused when it is nonsense and reads back when it is not,
// MLAG ids are unique and appear on the LAG and in the status, and the
// peer-link cannot be an MLAG interface. It leaves MLAG off.
func checkMLAG(sw Switch, caps Capabilities, p0, p1 string) []error {
	err := sw.SetMLAG(MLAGConfig{Enabled: false})
	if probs := wantSupport(caps.MLAG, err, "SetMLAG", "Capabilities.MLAG"); len(probs) > 0 || !caps.MLAG {
		return probs
	}
	var probs []error
	bad := func(f string, a ...any) { probs = append(probs, fmt.Errorf(f, a...)) }
	if !caps.LAGs {
		return append(probs, fmt.Errorf("Capabilities.MLAG without Capabilities.LAGs: an MLAG interface is a LAG"))
	}
	defer func() {
		_ = sw.SetMLAG(MLAGConfig{Enabled: false})
		_ = sw.DelLAG("po2")
		_ = sw.DelLAG("po3")
	}()

	if err := sw.SetMLAG(MLAGConfig{Enabled: true, PeerLink: "swp-nonexistent"}); err == nil {
		bad("SetMLAG accepted a peer-link that does not exist")
	}
	if err := sw.SetMLAG(MLAGConfig{Enabled: true, PeerLink: p0, PeerAddress: "bogus"}); err == nil {
		bad("SetMLAG accepted peer address %q", "bogus")
	}
	if err := sw.SetMLAG(MLAGConfig{Enabled: true, PeerLink: p0, PeerAddress: "192.0.2.2", Priority: 100}); err != nil {
		return append(probs, fmt.Errorf("SetMLAG(peer-link %s): %w", p0, err))
	}
	if st, err := sw.MLAG(); err != nil {
		bad("MLAG: %v", err)
	} else if !st.Enabled || st.PeerLink != p0 {
		bad("MLAG reports enabled=%v peer-link %q after SetMLAG(on, %s)", st.Enabled, st.PeerLink, p0)
	}

	if err := sw.AddLAG("po2", false); err != nil {
		return append(probs, fmt.Errorf("AddLAG(po2): %w", err))
	}
	if err := sw.SetLAGMembers("po2", []string{p1}); err != nil {
		bad("SetLAGMembers(po2, %s): %v", p1, err)
	}
	if err := sw.SetLAGMLAG("po2", MLAGMaxID+1); err == nil {
		bad("SetLAGMLAG accepted id %d", MLAGMaxID+1)
	}
	if err := sw.SetLAGMLAG("po2", 5); err != nil {
		return append(probs, fmt.Errorf("SetLAGMLAG(po2, 5): %w", err))
	}
	if err := sw.AddLAG("po3", false); err == nil {
		if err := sw.SetLAGMLAG("po3", 5); err == nil {
			bad("SetLAGMLAG gave po3 the id po2 already has")
		}
	}
	if err := sw.SetMLAG(MLAGConfig{Enabled: true, PeerLink: "po2"}); err == nil {
		bad("SetMLAG made MLAG interface po2 the peer-link")
	}
	if ls, err := sw.LAGs(); err == nil {
		for _, l := range ls {
			if l.Name == "po2" && l.MLAG != 5 {
				bad("po2 reports MLAG id %d, set 5", l.MLAG)
			}
		}
	}
	st, err := sw.MLAG()
	if err != nil {
		return append(probs, fmt.Errorf("MLAG: %w", err))
	}
	switch st.Role {
	case "primary", "secondary", "none":
	default:
		bad("MLAG role %q", st.Role)
	}
	var found bool
	for _, i := range st.Interfaces {
		if i.LAG != "po2" {
			continue
		}
		found = true
		if i.ID != 5 {
			bad("MLAG lists po2 with id %d, set 5", i.ID)
		}
		switch i.State {
		case "active", "local", "peer", "down", "disabled":
		default:
			bad("MLAG interface po2 in state %q", i.State)
		}
	}
	if !found {
		bad("po2 has MLAG id 5 but MLAG does not list it")
	}
	if err := sw.SetLAGMLAG("po2", 0); err != nil {
		bad("SetLAGMLAG(po2, 0): %v", err)
	} else if st, err := sw.MLAG(); err == nil {
		for _, i := range st.Interfaces {
			if i.LAG == "po2" {
				bad("po2 is still an MLAG interface after SetLAGMLAG(po2, 0)")
			}
		}
	}
	return probs
}
