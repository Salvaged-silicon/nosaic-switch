package mem

import (
	"fmt"
	"sort"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// MLAG, as one switch of a pair sees it with no peer to hear: the
// configuration is held and checked, the role is "none", and every MLAG
// interface is up on this side only. The chip datapaths run the protocol.

func (s *Switch) SetMLAG(cfg switchapi.MLAGConfig) error {
	if !s.cfg.Caps.MLAG {
		return switchapi.Unsupported("mlag")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if !cfg.Enabled {
		s.mlag = switchapi.MLAGConfig{}
		return nil
	}
	if err := switchapi.ValidMLAG(cfg); err != nil {
		return err
	}
	p, err := s.lookup(cfg.PeerLink)
	if err != nil {
		return err
	}
	if p.lag != "" {
		return fmt.Errorf("%s is a member of %s; make %s the peer-link instead", cfg.PeerLink, p.lag, p.lag)
	}
	if l, ok := s.lags[cfg.PeerLink]; ok && l.mlag != 0 {
		return fmt.Errorf("%s is MLAG interface %d; the peer-link cannot be one", cfg.PeerLink, l.mlag)
	}
	s.mlag = cfg
	return nil
}

func (s *Switch) SetLAGMLAG(name string, id int) error {
	if !s.cfg.Caps.MLAG {
		return switchapi.Unsupported("mlag")
	}
	if id < 0 || id > switchapi.MLAGMaxID {
		return fmt.Errorf("mlag id %d: must be 1 to %d, or 0 for none", id, switchapi.MLAGMaxID)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	l, ok := s.lags[name]
	if !ok {
		return fmt.Errorf("%s does not exist", name)
	}
	if id != 0 {
		if s.mlag.Enabled && s.mlag.PeerLink == name {
			return fmt.Errorf("%s is the peer-link; it cannot be an MLAG interface", name)
		}
		for n, o := range s.lags {
			if n != name && o.mlag == id {
				return fmt.Errorf("mlag id %d is already %s", id, n)
			}
		}
	}
	l.mlag = id
	return nil
}

func (s *Switch) MLAG() (switchapi.MLAGStatus, error) {
	if !s.cfg.Caps.MLAG {
		return switchapi.MLAGStatus{}, switchapi.Unsupported("mlag")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	out := switchapi.MLAGStatus{
		Enabled:  s.mlag.Enabled,
		Role:     "none",
		PeerLink: s.mlag.PeerLink,
		SystemID: "020000000001",
	}
	if p, ok := s.byName[s.mlag.PeerLink]; ok {
		out.PeerLinkUp = p.adminUp
	}
	for name, l := range s.lags {
		if l.mlag == 0 {
			continue
		}
		local := false
		for _, m := range l.members {
			local = local || s.byName[m].adminUp
		}
		st := "down"
		if local {
			st = "local"
		}
		out.Interfaces = append(out.Interfaces, switchapi.MLAGInterface{
			LAG: name, ID: l.mlag, Local: local, State: st})
	}
	sort.Slice(out.Interfaces, func(i, j int) bool { return out.Interfaces[i].ID < out.Interfaces[j].ID })
	return out, nil
}
