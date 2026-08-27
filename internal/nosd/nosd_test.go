package nosd_test

import (
	"errors"
	"net/netip"
	"path/filepath"
	"testing"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/mem"
	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/server"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// serve starts a nosd on a temporary socket and returns a connected client.
func serve(t *testing.T, sw switchapi.Switch) *client.Client {
	t.Helper()
	sock := filepath.Join(t.TempDir(), "nosd.sock")
	go func() { _ = server.New(sw, nil).Listen(sock) }()

	deadline := time.Now().Add(5 * time.Second)
	for {
		c, err := client.Dial(sock)
		if err == nil {
			t.Cleanup(func() { _ = c.Close() })
			return c
		}
		if time.Now().After(deadline) {
			t.Fatalf("nosd did not start: %v", err)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

// The whole point of the client implementing switchapi: the contract can be
// checked across the socket, not merely in-process. A protocol can round-trip
// every value correctly and still lose something the contract depends on.
func TestConformanceOverTheSocket(t *testing.T) {
	sw := mem.New(mem.Config{Ports: 4, Caps: mem.DefaultCaps()})
	switchapi.Conform(t, serve(t, sw))
}

func TestConformanceOverTheSocketWithoutL3(t *testing.T) {
	caps := mem.DefaultCaps()
	caps.L3 = false
	caps.ECMP = false
	switchapi.Conform(t, serve(t, mem.New(mem.Config{Ports: 4, Caps: caps})))
}

// The thing most likely to be lost in serialisation, and the most damaging to
// lose. A caller that cannot tell "this hardware cannot" from "this went
// wrong" will retry forever something that will never work.
func TestUnsupportedSurvivesTheWire(t *testing.T) {
	caps := mem.DefaultCaps()
	caps.ECMP = false
	caps.MaxECMP = 1
	c := serve(t, mem.New(mem.Config{Ports: 2, Caps: caps}))

	if err := c.AddAddress("swp1", netip.MustParsePrefix("10.0.0.1/24")); err != nil {
		t.Fatal(err)
	}
	err := c.AddRoute(switchapi.Route{
		Prefix: netip.MustParsePrefix("10.9.0.0/24"),
		NextHops: []switchapi.NextHop{
			{Via: netip.MustParseAddr("10.0.0.2"), Port: "swp1"},
			{Via: netip.MustParseAddr("10.0.0.3"), Port: "swp1"},
		},
	})
	if err == nil {
		t.Fatal("a multipath route should have been refused")
	}
	if !errors.Is(err, switchapi.ErrUnsupported) {
		t.Fatalf("the refusal did not survive the wire as ErrUnsupported: %v", err)
	}
}

// An ordinary error must not be mistaken for an unsupported operation, or a
// caller would give up on something that would work if retried.
func TestOrdinaryErrorIsNotUnsupported(t *testing.T) {
	c := serve(t, mem.New(mem.Config{Ports: 2, Caps: mem.DefaultCaps()}))
	_, err := c.PortStatus("swp-nonexistent")
	if err == nil {
		t.Fatal("an unknown port should be an error")
	}
	if errors.Is(err, switchapi.ErrUnsupported) {
		t.Fatal("an unknown port is a mistake, not an unsupported capability")
	}
}

func TestCapabilitiesCrossTheWire(t *testing.T) {
	want := mem.DefaultCaps()
	c := serve(t, mem.New(mem.Config{Ports: 8, Caps: want}))
	got := c.Capabilities()
	if got.Driver != want.Driver || got.MaxECMP != want.MaxECMP || got.L3 != want.L3 {
		t.Fatalf("capabilities did not survive: got %+v", got)
	}
	if got.Contract != switchapi.Version {
		t.Fatalf("contract version = %q, want %q", got.Contract, switchapi.Version)
	}
}

func TestRoutesRoundTrip(t *testing.T) {
	c := serve(t, mem.New(mem.Config{Ports: 2, Caps: mem.DefaultCaps()}))
	r := switchapi.Route{
		Prefix: netip.MustParsePrefix("192.0.2.0/24"),
		NextHops: []switchapi.NextHop{
			{Via: netip.MustParseAddr("10.0.0.1"), Port: "swp1"},
			{Via: netip.MustParseAddr("10.0.0.2"), Port: "swp2"},
		},
	}
	if err := c.AddRoute(r); err != nil {
		t.Fatal(err)
	}
	got, err := c.Routes()
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 || got[0].Prefix != r.Prefix {
		t.Fatalf("routes = %+v", got)
	}
	// Both next-hops must come back: a protocol that dropped one would look
	// correct and quietly halve the paths, which is the ECMP bug again.
	if len(got[0].NextHops) != 2 {
		t.Fatalf("expected 2 next-hops, got %d", len(got[0].NextHops))
	}
}
