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
