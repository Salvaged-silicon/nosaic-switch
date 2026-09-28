package virt

import (
	"strconv"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// MAC aging is the bridge's ageing_time, in centiseconds. The setting is kept
// here as well, because the kernel cannot be told "never": an ageing_time of
// 0 turns learning off altogether, and the bridge floods every frame. Never
// is given to it as the longest age the contract allows.
func (s *Switch) applyAging() error {
	if s.aging == nil || !exists(bridgeName) {
		return nil
	}
	sec := *s.aging
	if sec == 0 {
		sec = 1000000
	}
	_, err := ipCmd("link", "set", bridgeName, "type", "bridge", "ageing_time", strconv.Itoa(sec*100))
	return err
}

func (s *Switch) SetMACAging(seconds int) error {
	if !s.vlans {
		return switchapi.Unsupported("mac aging")
	}
	if err := switchapi.ValidMACAging(seconds); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.aging = &seconds
	return s.applyAging()
}

func (s *Switch) MACAging() (int, error) {
	if !s.vlans {
		return 0, switchapi.Unsupported("mac aging")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.aging == nil {
		return switchapi.DefaultMACAging, nil
	}
	return *s.aging, nil
}
