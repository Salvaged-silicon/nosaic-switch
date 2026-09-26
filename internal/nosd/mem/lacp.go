package mem

import (
	"fmt"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// lagOptions and lacpPriority are what a LAG's settings read back as: the
// defaults filled in, so a reader sees what is in force, not what was left out.
func lagOptions(o switchapi.LAGOptions) switchapi.LAGOptions {
	if o.Rate == "" {
		o.Rate = "fast"
	}
	if o.PortPriority == 0 {
		o.PortPriority = switchapi.LACPDefaultPriority
	}
	return o
}

func lacpPriority(p int) int {
	if p == 0 {
		return switchapi.LACPDefaultPriority
	}
	return p
}

func (s *Switch) SetLAGOptions(name string, o switchapi.LAGOptions) error {
	if !s.cfg.Caps.LAGs {
		return switchapi.Unsupported("link aggregation")
	}
	if err := switchapi.ValidLAGOptions(o); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	l, ok := s.lags[name]
	if !ok {
		return fmt.Errorf("%s does not exist", name)
	}
	l.opts = o
	return nil
}

func (s *Switch) SetLACPSystemPriority(p int) error {
	if !s.cfg.Caps.LACP {
		return switchapi.Unsupported("lacp")
	}
	if err := switchapi.ValidLACPPriority(p); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.lacpPri = p
	return nil
}
