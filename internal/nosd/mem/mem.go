// Package mem is an in-memory reference implementation of switchapi.
//
// It forwards nothing. Its job is to define the contract's semantics
// concretely — what "add a route then list routes" is supposed to do — and to
// let the config model, the CLI and the conformance suite be exercised with no
// hardware, no privileges and no network namespaces.
//
// A real datapath is judged against this: same calls, same observable
// behaviour, different mechanism underneath.
package mem

import (
	"fmt"
	"net/netip"
	"sort"
	"sync"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// Config shapes the simulated switch. Capabilities are settable so that tests
// can construct an implementation with a particular feature absent — including
// a dishonest one, which is how the conformance suite is itself tested.
type Config struct {
	Ports int
	Caps  switchapi.Capabilities

	// AllowECMPAnyway makes AddRoute accept multipath routes even when
	// Capabilities says ECMP is unavailable. Nothing real should do this; it
	// exists so the conformance suite can be shown to catch it.
	AllowECMPAnyway bool
}

// DefaultCaps is what a fully featured software datapath claims.
func DefaultCaps() switchapi.Capabilities {
	return switchapi.Capabilities{
		Contract:   switchapi.Version,
		Driver:     "mem",
		MaxPorts:   64,
		VLANs:      true,
		MaxVLANs:   4094,
		L2Learning: true,
		L3:         true,
		IPv6:       true,
		ECMP:       true,
		MaxECMP:    8,
		Counters:   true,
	}
}

type port struct {
	name    string
	adminUp bool
	mtu     int
	vlans   map[int]bool
	addrs   map[netip.Prefix]bool
}

// Switch is an in-memory switchapi.Switch.
type Switch struct {
	mu      sync.Mutex
	cfg     Config
	started bool
	ports   []*port
	byName  map[string]*port
	vlans   map[int]bool
	routes  map[netip.Prefix]switchapi.Route
}

// New builds a simulated switch.
func New(cfg Config) *Switch {
	if cfg.Ports <= 0 {
		cfg.Ports = 4
	}
	if cfg.Caps.Contract == "" {
		cfg.Caps.Contract = switchapi.Version
	}
	if cfg.Caps.Driver == "" {
		cfg.Caps.Driver = "mem"
	}
	s := &Switch{
		cfg:    cfg,
		byName: map[string]*port{},
		vlans:  map[int]bool{},
		routes: map[netip.Prefix]switchapi.Route{},
	}
	for i := 1; i <= cfg.Ports; i++ {
		p := &port{
			name:  fmt.Sprintf("swp%d", i),
			mtu:   1500,
			vlans: map[int]bool{},
			addrs: map[netip.Prefix]bool{},
		}
		s.ports = append(s.ports, p)
		s.byName[p.name] = p
	}
	return s
}

func (s *Switch) Capabilities() switchapi.Capabilities { return s.cfg.Caps }

func (s *Switch) Start() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.started = true
	return nil
}

func (s *Switch) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.started = false
	return nil
}

func (s *Switch) Ports() ([]switchapi.Port, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]switchapi.Port, len(s.ports))
	for i, p := range s.ports {
		out[i] = switchapi.Port{Name: p.name, Index: i}
	}
	return out, nil
}

// lookup is the single place an unknown port becomes an error. Returning a
// zero value for an unknown port is how a typo becomes a silent no-op.
func (s *Switch) lookup(name string) (*port, error) {
	p, ok := s.byName[name]
	if !ok {
		return nil, fmt.Errorf("no such port %q", name)
	}
	return p, nil
}

func (s *Switch) PortStatus(name string) (switchapi.PortStatus, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	p, err := s.lookup(name)
	if err != nil {
		return switchapi.PortStatus{}, err
	}
	return switchapi.PortStatus{
		Name: p.name, AdminUp: p.adminUp,
		// A simulated port carries no cable, so oper follows admin.
		OperUp: p.adminUp, SpeedMbps: 10000, FullDuplex: true, MTU: p.mtu,
	}, nil
}

func (s *Switch) SetPortAdmin(name string, up bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	p, err := s.lookup(name)
	if err != nil {
		return err
	}
	p.adminUp = up
	return nil
}

func (s *Switch) SetPortMTU(name string, mtu int) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	p, err := s.lookup(name)
	if err != nil {
		return err
	}
	if mtu < 68 || mtu > 9216 {
		return fmt.Errorf("mtu %d out of range", mtu)
	}
	p.mtu = mtu
	return nil
}

func (s *Switch) PortCounters(name string) (switchapi.Counters, error) {
	if !s.cfg.Caps.Counters {
		return switchapi.Counters{}, switchapi.Unsupported("counters")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.lookup(name); err != nil {
		return switchapi.Counters{}, err
	}
	return switchapi.Counters{}, nil
}

func (s *Switch) AddVLAN(vid int) error {
	if !s.cfg.Caps.VLANs {
		return switchapi.Unsupported("vlans")
	}
	if vid < 1 || vid > 4094 {
		return fmt.Errorf("vlan %d out of range", vid)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.vlans[vid] = true
	return nil
}

func (s *Switch) DelVLAN(vid int) error {
	if !s.cfg.Caps.VLANs {
		return switchapi.Unsupported("vlans")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.vlans, vid)
	return nil
}

func (s *Switch) SetPortVLAN(name string, vid int, tagged bool) error {
	if !s.cfg.Caps.VLANs {
		return switchapi.Unsupported("vlans")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	p, err := s.lookup(name)
	if err != nil {
		return err
	}
	if !s.vlans[vid] {
		return fmt.Errorf("vlan %d does not exist", vid)
	}
	p.vlans[vid] = tagged
	return nil
}

func (s *Switch) FDB() ([]switchapi.FDBEntry, error) {
	if !s.cfg.Caps.L2Learning {
		return nil, switchapi.Unsupported("l2 learning")
	}
	return nil, nil
}

func (s *Switch) AddAddress(name string, addr netip.Prefix) error {
	if !s.cfg.Caps.L3 {
		return switchapi.Unsupported("l3")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	p, err := s.lookup(name)
	if err != nil {
		return err
	}
	p.addrs[addr] = true
	return nil
}

func (s *Switch) DelAddress(name string, addr netip.Prefix) error {
	if !s.cfg.Caps.L3 {
		return switchapi.Unsupported("l3")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	p, err := s.lookup(name)
	if err != nil {
		return err
	}
	delete(p.addrs, addr)
	return nil
}

func (s *Switch) AddRoute(r switchapi.Route) error {
	if !s.cfg.Caps.L3 {
		return switchapi.Unsupported("l3")
	}
	if len(r.NextHops) == 0 {
		return fmt.Errorf("route %s has no next-hops", r.Prefix)
	}
	// Refuse rather than silently installing one path.
	if len(r.NextHops) > 1 && !s.cfg.Caps.SupportsECMP(len(r.NextHops)) && !s.cfg.AllowECMPAnyway {
		return switchapi.Unsupported(fmt.Sprintf("multipath route with %d next-hops", len(r.NextHops)))
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.routes[r.Prefix] = r
	return nil
}

func (s *Switch) DelRoute(p netip.Prefix) error {
	if !s.cfg.Caps.L3 {
		return switchapi.Unsupported("l3")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.routes, p)
	return nil
}

func (s *Switch) Routes() ([]switchapi.Route, error) {
	if !s.cfg.Caps.L3 {
		return nil, switchapi.Unsupported("l3")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]switchapi.Route, 0, len(s.routes))
	for _, r := range s.routes {
		out = append(out, r)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Prefix.String() < out[j].Prefix.String() })
	return out, nil
}
