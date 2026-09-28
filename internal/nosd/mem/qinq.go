package mem

import (
	"fmt"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

func tpidOr(t int) int {
	if t == 0 {
		return switchapi.DefaultTPID
	}
	return t
}

func (s *Switch) SetPortTunnel(name string, svid int) error {
	if !s.cfg.Caps.QinQ {
		return switchapi.Unsupported("qinq")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	p, err := s.lookup(name)
	if err != nil {
		return err
	}
	if p.lag != "" {
		return fmt.Errorf("%s is a member of %s; make %s the tunnel port instead", name, p.lag, p.lag)
	}
	if !s.vlans[svid] {
		return fmt.Errorf("vlan %d does not exist", svid)
	}
	// Its only membership.
	for v := range p.vlans {
		delete(p.vlans, v)
	}
	p.vlans[svid] = false
	p.tunnel = svid
	return nil
}

func (s *Switch) SetPortTPID(name string, tpid int) error {
	if !s.cfg.Caps.QinQ {
		return switchapi.Unsupported("qinq")
	}
	if err := switchapi.ValidTPID(tpid); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	p, err := s.lookup(name)
	if err != nil {
		return err
	}
	p.tpid = tpid
	return nil
}
