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

// STPTimes fills in a configuration's defaulted bridge times, in seconds.
func STPTimes(c STPConfig) (hello, fwd, maxAge int) {
	hello, fwd, maxAge = c.HelloTime, c.ForwardDelay, c.MaxAge
	if hello == 0 {
		hello = 2
	}
	if fwd == 0 {
		fwd = 15
	}
	if maxAge == 0 {
		maxAge = 20
	}
	return hello, fwd, maxAge
}

// ValidSTPTimes checks the bridge times against 802.1D-2004 17.14: each in its
// range, and max age between twice (hello + 1) and twice (forward delay - 1),
// which is what keeps a BPDU from outliving the tree it describes.
func ValidSTPTimes(c STPConfig) error {
	h, f, m := STPTimes(c)
	switch {
	case h < 1 || h > 10:
		return fmt.Errorf("stp hello %d: must be 1 to 10 seconds", h)
	case f < 4 || f > 30:
		return fmt.Errorf("stp forward-delay %d: must be 4 to 30 seconds", f)
	case m < 6 || m > 40:
		return fmt.Errorf("stp max-age %d: must be 6 to 40 seconds", m)
	case m > 2*(f-1) || m < 2*(h+1):
		return fmt.Errorf("stp max-age %d with hello %d and forward-delay %d: 802.1D needs "+
			"2 x (forward-delay - 1) >= max-age >= 2 x (hello + 1)", m, h, f)
	}
	return nil
}

// ValidSTPPortPriority checks a port priority: 0 for the default, or 16 to 240
// in steps of 16 -- the top four bits of the port id.
func ValidSTPPortPriority(p int) error {
	if p != 0 && (p < 16 || p > 240 || p%16 != 0) {
		return fmt.Errorf("stp port priority %d: must be 16 to 240 in steps of 16", p)
	}
	return nil
}

// ValidLAGOptions checks a LAG's LACP options.
func ValidLAGOptions(o LAGOptions) error {
	if o.Rate != "" && o.Rate != "fast" && o.Rate != "slow" {
		return fmt.Errorf("lacp rate %q: fast or slow", o.Rate)
	}
	if o.PortPriority < 0 || o.PortPriority > 65535 {
		return fmt.Errorf("lacp port-priority %d: must be 1 to 65535", o.PortPriority)
	}
	return nil
}

// ValidLACPPriority checks the LACP system priority.
func ValidLACPPriority(p int) error {
	if p < 0 || p > 65535 {
		return fmt.Errorf("lacp system-priority %d: must be 1 to 65535", p)
	}
	return nil
}

// MLAGTimes fills in a configuration's defaulted timers.
func MLAGTimes(c MLAGConfig) (hello, dead, settle, port int) {
	hello, dead, settle, port = c.HelloMs, c.DeadMs, c.SettleMs, c.HeartbeatPort
	if hello == 0 {
		hello = 1000
	}
	if dead == 0 {
		dead = 3500
	}
	if settle == 0 {
		settle = 2500
	}
	if port == 0 {
		port = 47101
	}
	return hello, dead, settle, port
}

// ValidMLAGTimes checks MLAG's timers: a hello from 100 ms to 10 s, the dead
// interval at least twice the hello -- one hello lost must not lose the peer
// and the settle time and heartbeat port in range.
func ValidMLAGTimes(c MLAGConfig) error {
	h, d, s, p := MLAGTimes(c)
	switch {
	case h < 100 || h > 10000:
		return fmt.Errorf("mlag hello %d ms: must be 100 to 10000", h)
	case d < 2*h || d > 60000:
		return fmt.Errorf("mlag dead %d ms: must be at least twice the hello (%d) and at most 60000", d, h)
	case s < 0 || s > 60000:
		return fmt.Errorf("mlag settle %d ms: must be 0 to 60000", s)
	case p < 1 || p > 65535:
		return fmt.Errorf("mlag heartbeat-port %d: must be 1 to 65535", p)
	}
	return nil
}
