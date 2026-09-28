package virt

import "github.com/salvaged-silicon/nosaic-switch/internal/switchapi"

// QinQ is refused here, as the capability says. A Linux bridge's VLAN protocol
// is the bridge's, 802.1Q or 802.1ad for every port at once, so it cannot hold
// customer ports and 802.1Q trunks side by side the way a chip does, and a
// half-honest imitation would pass tests the hardware then fails.
func (s *Switch) SetPortTunnel(string, int) error { return switchapi.Unsupported("qinq") }
func (s *Switch) SetPortTPID(string, int) error   { return switchapi.Unsupported("qinq") }
