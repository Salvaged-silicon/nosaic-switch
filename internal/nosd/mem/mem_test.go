package mem

import (
	"errors"
	"net/netip"
	"strings"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// The reference implementation must pass the contract unchanged.
func TestConformance(t *testing.T) {
	switchapi.Conform(t, New(Config{Ports: 4, Caps: DefaultCaps()}))
}

// A datapath with a feature genuinely absent must also pass — by refusing
// correctly rather than by having nothing checked.
func TestConformanceWithoutL3(t *testing.T) {
	caps := DefaultCaps()
	caps.L3 = false
	caps.ECMP = false
	switchapi.Conform(t, New(Config{Ports: 4, Caps: caps}))
}

func TestConformanceWithoutVLANs(t *testing.T) {
	caps := DefaultCaps()
	caps.VLANs = false
	switchapi.Conform(t, New(Config{Ports: 4, Caps: caps}))
}

// A datapath honestly lacking ECMP passes: it refuses multipath.
func TestConformanceWithoutECMP(t *testing.T) {
	caps := DefaultCaps()
	caps.ECMP = false
	caps.MaxECMP = 1
	switchapi.Conform(t, New(Config{Ports: 4, Caps: caps}))
}

// The suite has to be shown to catch things, or passing it means nothing.
//
// This is the exact bug the contract exists to prevent, and the one the
// previous generation of this project shipped: a datapath that claims no
// multipath and quietly installs a single path anyway.
func TestSuiteCatchesSilentECMPDowngrade(t *testing.T) {
	caps := DefaultCaps()
	caps.ECMP = false
	caps.MaxECMP = 1
	liar := New(Config{Ports: 4, Caps: caps, AllowECMPAnyway: true})

	probs := switchapi.Check(liar)
	if !anyContains(probs, "capabilities and behaviour must agree") {
		t.Fatalf("the suite did not catch a silent ECMP downgrade; problems were: %v", probs)
	}
}

// The mirror image: claiming a capability and then refusing to perform it.
func TestSuiteCatchesFalseCapabilityClaim(t *testing.T) {
	caps := DefaultCaps()
	sw := &refusesVLANs{New(Config{Ports: 4, Caps: caps})}

	probs := switchapi.Check(sw)
	if !anyContains(probs, "although Capabilities.VLANs is true") {
		t.Fatalf("the suite did not catch a false capability claim; problems were: %v", probs)
	}
}

// And an implementation that refuses with the wrong error type, so callers
// cannot tell "this hardware cannot" from "this went wrong".
func TestSuiteCatchesWrongErrorType(t *testing.T) {
	caps := DefaultCaps()
	caps.Counters = false
	sw := &wrongError{New(Config{Ports: 4, Caps: caps})}

	probs := switchapi.Check(sw)
	if !anyContains(probs, "does not wrap ErrUnsupported") {
		t.Fatalf("the suite did not catch a non-ErrUnsupported refusal; problems were: %v", probs)
	}
}

func TestUnknownPortIsAnError(t *testing.T) {
	s := New(Config{Ports: 2, Caps: DefaultCaps()})
	if err := s.SetPortAdmin("swp99", true); err == nil {
		t.Fatal("configuring a port that does not exist must fail, not silently do nothing")
	}
}

func TestRouteRoundTrip(t *testing.T) {
	s := New(Config{Ports: 2, Caps: DefaultCaps()})
	_ = s.Start()
	r := switchapi.Route{
		Prefix:   netip.MustParsePrefix("192.0.2.0/24"),
		NextHops: []switchapi.NextHop{{Via: netip.MustParseAddr("10.0.0.1"), Port: "swp1"}},
	}
	if err := s.AddRoute(r); err != nil {
		t.Fatal(err)
	}
	got, err := s.Routes()
	if err != nil || len(got) != 1 || got[0].Prefix != r.Prefix {
		t.Fatalf("routes = %v, %v", got, err)
	}
	if err := s.DelRoute(r.Prefix); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.Routes(); len(got) != 0 {
		t.Fatalf("route still present after deletion: %v", got)
	}
}

// --- deliberately broken implementations, used only above ---

type refusesVLANs struct{ *Switch }

func (r *refusesVLANs) AddVLAN(int) error { return switchapi.Unsupported("vlans") }

type wrongError struct{ *Switch }

func (w *wrongError) PortCounters(string) (switchapi.Counters, error) {
	return switchapi.Counters{}, errors.New("something went wrong")
}

func anyContains(errs []error, sub string) bool {
	for _, e := range errs {
		if strings.Contains(e.Error(), sub) {
			return true
		}
	}
	return false
}
