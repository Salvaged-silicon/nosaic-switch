package switchapi

import (
	"fmt"
	"net"
	"net/netip"
)

// ValidSTPPriority checks a bridge priority: 802.1D-2004 carries it in the top
// four bits of the bridge ID, so it is 0 to 61440 in steps of 4096. Every
// datapath refuses the same values with the same words.
func ValidSTPPriority(p int) error {
	if p < 0 || p > 61440 || p%4096 != 0 {
		return fmt.Errorf("stp priority %d: must be 0 to 61440 in steps of 4096", p)
	}
	return nil
}

// ValidSTPCost checks a port path cost: 0 for the default from the link's
// speed, or 1 to 200000000.
func ValidSTPCost(c int) error {
	if c < 0 || c > 200000000 {
		return fmt.Errorf("stp cost %d: must be 1 to 200000000, or 0 for the speed's default", c)
	}
	return nil
}

// STPDefaultCost is 802.1D-2004's recommended path cost for a link speed in
// Mb/s: 20000000 divided by the speed, so 1 Gb/s is 20000, 10 Gb/s 2000 and
// 40 Gb/s 500.
func STPDefaultCost(mbps int) int {
	if mbps <= 0 {
		return 200000000
	}
	c := 20000000 / mbps
	if c < 1 {
		c = 1
	}
	return c
}

// ValidMLAG checks an MLAG configuration that turns MLAG on, the parts that
// do not depend on the datapath: a peer-link named, a peer address that is an
// address if given, a priority in 16 bits.
func ValidMLAG(c MLAGConfig) error {
	if c.PeerLink == "" {
		return fmt.Errorf("mlag needs a peer-link")
	}
	if c.PeerAddress != "" {
		if _, err := netip.ParseAddr(c.PeerAddress); err != nil {
			return fmt.Errorf("mlag peer-address %q is not an address", c.PeerAddress)
		}
	}
	if c.Priority < 0 || c.Priority > 65535 {
		return fmt.Errorf("mlag priority %d: must be 0 to 65535", c.Priority)
	}
	return nil
}

// ValidVirtualMAC checks a virtual gateway MAC: six octets, and unicast -- a
// multicast source address is dropped by every host that receives one.
func ValidVirtualMAC(mac string) error {
	hw, err := net.ParseMAC(mac)
	if err != nil || len(hw) != 6 {
		return fmt.Errorf("virtual mac %q is not a MAC address", mac)
	}
	if hw[0]&1 != 0 {
		return fmt.Errorf("virtual mac %s is multicast; it must be unicast", mac)
	}
	return nil
}

// ValidVirtualGateway checks a gateway address: IPv4, for now, since IPv6
// needs neighbour discovery answered with the virtual MAC too.
func ValidVirtualGateway(addr netip.Prefix) error {
	if !addr.Addr().Is4() {
		return fmt.Errorf("virtual gateway %s: IPv4 only", addr)
	}
	if addr.Bits() < 1 || addr.Bits() > 32 {
		return fmt.Errorf("virtual gateway %s: prefix length", addr)
	}
	return nil
}
