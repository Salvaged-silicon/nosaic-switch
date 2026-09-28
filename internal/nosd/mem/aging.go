package mem

import "github.com/salvaged-silicon/nosaic-switch/internal/switchapi"

func (s *Switch) SetMACAging(seconds int) error {
	if !s.cfg.Caps.MACAging {
		return switchapi.Unsupported("mac aging")
	}
	if err := switchapi.ValidMACAging(seconds); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.aging = &seconds
	return nil
}

func (s *Switch) MACAging() (int, error) {
	if !s.cfg.Caps.MACAging {
		return 0, switchapi.Unsupported("mac aging")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.aging == nil {
		return switchapi.DefaultMACAging, nil
	}
	return *s.aging, nil
}
