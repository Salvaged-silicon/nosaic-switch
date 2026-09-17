package switchapi

import (
	"errors"
	"fmt"
	"net/netip"
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
	if err := sw.SetPortVLAN(p0, 100, true); err != nil {
		probs = append(probs, fmt.Errorf("SetPortVLAN: %w", err))
	}
	if err := sw.DelVLAN(100); err != nil {
		probs = append(probs, fmt.Errorf("DelVLAN: %w", err))
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
