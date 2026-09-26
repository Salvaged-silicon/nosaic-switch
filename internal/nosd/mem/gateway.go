package mem

import (
	"fmt"
	"net/netip"
	"sort"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// Virtual gateways, held and checked: a shared address on an SVI, answered
// with the virtual MAC. The chip datapaths answer and route for it.

func (s *Switch) mac() string {
	if s.vmac == "" {
		return switchapi.DefaultVirtualMAC
	}
	return s.vmac
}

func (s *Switch) SetVirtualMAC(mac string) error {
	if !s.cfg.Caps.VirtualGateway {
		return switchapi.Unsupported("virtual gateway")
	}
	if err := switchapi.ValidVirtualMAC(mac); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.vmac = mac
	return nil
}

func (s *Switch) AddVirtualGateway(svi string, addr netip.Prefix) error {
	if !s.cfg.Caps.VirtualGateway {
		return switchapi.Unsupported("virtual gateway")
	}
	if err := switchapi.ValidVirtualGateway(addr); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	found := false
	for vid := range s.svis {
		found = found || switchapi.SVIName(vid) == svi
	}
	if !found {
		return fmt.Errorf("%s is not a routed vlan interface", svi)
	}
	if s.gws == nil {
		s.gws = map[switchapi.VirtualGateway]bool{}
	}
	s.gws[switchapi.VirtualGateway{SVI: svi, Address: addr}] = true
	return nil
}

func (s *Switch) DelVirtualGateway(svi string, addr netip.Prefix) error {
	if !s.cfg.Caps.VirtualGateway {
		return switchapi.Unsupported("virtual gateway")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.gws, switchapi.VirtualGateway{SVI: svi, Address: addr})
	return nil
}

func (s *Switch) VirtualGateways() ([]switchapi.VirtualGateway, error) {
	if !s.cfg.Caps.VirtualGateway {
		return nil, switchapi.Unsupported("virtual gateway")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []switchapi.VirtualGateway
	for g := range s.gws {
		g.MAC = s.mac()
		out = append(out, g)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].SVI != out[j].SVI {
			return out[i].SVI < out[j].SVI
		}
		return out[i].Address.String() < out[j].Address.String()
	})
	return out, nil
}
