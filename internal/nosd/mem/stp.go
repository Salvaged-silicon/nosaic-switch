package mem

import (
	"fmt"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// Spanning tree, as one bridge alone sees it: with no neighbour to hear a BPDU
// from, it is the root, and every switched interface is a designated port,
// forwarding. That is the reference answer; the chip datapaths run the real
// protocol.

type stpState struct {
	cfg   switchapi.STPConfig
	ports map[string]switchapi.STPPortConfig
}

func (s *Switch) stp() *stpState {
	if s.stpst == nil {
		s.stpst = &stpState{
			cfg:   switchapi.STPConfig{Priority: switchapi.STPDefaultPriority},
			ports: map[string]switchapi.STPPortConfig{},
		}
	}
	return s.stpst
}

func (s *Switch) SetSTP(cfg switchapi.STPConfig) error {
	if !s.cfg.Caps.STP {
		return switchapi.Unsupported("spanning tree")
	}
	if err := switchapi.ValidSTPPriority(cfg.Priority); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.stp().cfg = cfg
	return nil
}

func (s *Switch) SetSTPPort(name string, cfg switchapi.STPPortConfig) error {
	if !s.cfg.Caps.STP {
		return switchapi.Unsupported("spanning tree")
	}
	if err := switchapi.ValidSTPCost(cfg.Cost); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	p, err := s.lookup(name)
	if err != nil {
		return err
	}
	if p.lag != "" {
		return fmt.Errorf("%s is a member of %s; configure %s instead", name, p.lag, p.lag)
	}
	s.stp().ports[name] = cfg
	return nil
}

func (s *Switch) STP() (switchapi.STPStatus, error) {
	if !s.cfg.Caps.STP {
		return switchapi.STPStatus{}, switchapi.Unsupported("spanning tree")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	st := s.stp()
	id := fmt.Sprintf("%04x.020000000001", st.cfg.Priority)
	out := switchapi.STPStatus{
		Enabled:  st.cfg.Enabled,
		Priority: st.cfg.Priority,
		BridgeID: id,
		RootID:   id,
	}
	for _, p := range s.switchable() {
		if len(p.vlans) == 0 || p.lag != "" {
			continue
		}
		pc := st.ports[p.name]
		cost := pc.Cost
		if cost == 0 {
			cost = switchapi.STPDefaultCost(10000)
		}
		out.Ports = append(out.Ports, switchapi.STPPort{
			Port: p.name, Role: "designated", State: "forwarding",
			Edge: pc.Edge, Cost: cost,
		})
	}
	return out, nil
}
