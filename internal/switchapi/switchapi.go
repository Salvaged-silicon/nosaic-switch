// Package switchapi is the northbound contract every datapath implements.
//
// NOSaic runs a different daemon per ASIC family — a vendor SDK on one chip,
// an in-kernel driver on another, veth pairs on a virtual board. What must not
// differ is what a user types, what the config file says, or what the CLI can
// promise. This package is that boundary.
//
// # Why it is defined against a virtual board
//
// The contract is written before any real silicon implements it, and its first
// implementation drives veth pairs. That is deliberate. An interface designed
// while staring at one chip's registers becomes a description of that chip,
// and every subsequent ASIC contorts to fit a model chosen for reasons that no
// longer apply. The previous generation of this project ended up with a
// "datapath" package that included a Broadcom chip header and hard-coded 52
// ports; nobody decided that, it accrued.
//
// # Capabilities, and refusing rather than degrading
//
// No two switch chips support the same feature set: ECMP width, ACL capacity,
// route table size and breakout support all vary, and some boards cannot do a
// thing at all. So every implementation declares what it supports, and an
// operation outside that set returns ErrUnsupported rather than quietly doing
// something smaller.
//
// This is the failure mode worth naming. In the previous system, one board's
// L3 configuration was copied to a second board with ECMP support silently
// removed; nothing recorded the difference, and a route with two next-hops
// simply installed one. A switch that quietly forwards over half the paths you
// asked for is worse than one that refuses, because nothing tells you.
package switchapi

import (
	"errors"
	"fmt"
	"net/netip"
)

// Version is the contract version. It is versioned from the first commit
// because M6 will be the first thing to discover what is missing from it, and
// an implementation must be able to say which contract it was written against.
//
// Adding a capability or an optional method is a minor bump. Changing the
// meaning of an existing call is a major one, and requires updating every
// implementation in the same commit — including the virtual one, which is what
// keeps that honest.
//
// 1.1 added access lists: ACLs, SetACL and DelACL, gated by Capabilities.ACL
// and ACL6.
//
// 1.2 made VLANs something a caller can see and undo, and added routed VLAN
// interfaces: VLANs, DelPortVLAN, AddSVI and DelSVI, the last two gated by
// Capabilities.SVIs. It also pinned down what SetPortVLAN already implied but
// never said: an untagged membership is the port's native VLAN, and a port has
// at most one.
//
// 1.3 added link aggregation: LAGs, AddLAG, SetLAGMembers and DelLAG, gated by
// Capabilities.LAGs, with LACP gated by Capabilities.LACP. A LAG is an
// interface named po<N>, accepted wherever a port name is.
const Version = "1.3"

// ErrUnsupported is returned for an operation this hardware cannot perform.
// Callers should report it, never work around it silently.
var ErrUnsupported = errors.New("not supported by this datapath")

// Unsupported builds an ErrUnsupported naming the specific thing, so the
// message reaches the operator rather than only the log.
func Unsupported(what string) error {
	return fmt.Errorf("%s: %w", what, ErrUnsupported)
}

// Capabilities is what an implementation can do. A zero value means "cannot",
// so a new field defaults to absent rather than to a false promise.
type Capabilities struct {
	// Contract is the switchapi version this implementation targets.
	Contract string

	// Driver names the implementation, e.g. "virt", "td2", "prestera".
	Driver string

	MaxPorts int

	VLANs    bool
	MaxVLANs int
	// SVIs is routed VLAN interfaces: an L3 interface named vlan<VID> that
	// routes for every port in the VLAN, which is what Cisco calls an SVI.
	SVIs bool

	// LAGs is link aggregation: port-channels the chip load-shares across.
	// LACP is the negotiated kind; without it a LAG is static, every member
	// with link carries traffic.
	LAGs          bool
	MaxLAGs       int
	MaxLAGMembers int
	LACP          bool

	L2Learning bool
	MaxFDB     int

	L3      bool
	MaxV4   int // IPv4 route capacity, 0 if unknown
	MaxV6   int
	IPv6    bool
	ECMP    bool
	MaxECMP int // maximum next-hops per route; 1 means no multipath

	ACL       bool
	ACLSlices int
	// ACLEntries is how many rules the chip's field group can hold in
	// total, which is what an operator sizing an access list needs; slices
	// are how the silicon is organised, not how many rules fit.
	ACLEntries int
	// ACL6 and ACL6Entries are the same for IPv6, which is a separate group
	// on every chip so far: a 128-bit address does not share a key with a
	// 32-bit one, and a chip can have room for one family and not the other.
	ACL6        bool
	ACL6Entries int

	Counters bool
	SFP      bool
	Breakout bool
}

// Supports reports whether ECMP of the requested width is available, which is
// the check callers most often get wrong by assuming.
func (c Capabilities) SupportsECMP(paths int) bool {
	if paths <= 1 {
		return true
	}
	return c.ECMP && (c.MaxECMP == 0 || paths <= c.MaxECMP)
}

// Port is a front-panel port. Names are the board's — swp1, swp49 — never
// ASIC or physical numbers: translating between them is the board port map's
// job, and doing it in exactly one place is what keeps the rest portable.
type Port struct {
	Name  string
	Index int // opaque implementation handle, not for display
}

// AdminState is what the operator asked for; OperState is what the hardware
// reports. Keeping them separate matters: a port that is admin-up and
// oper-down is a cable problem, and a port that is admin-down is not a fault.
type PortStatus struct {
	Name       string
	AdminUp    bool
	OperUp     bool
	SpeedMbps  int
	FullDuplex bool
	MTU        int
}

// Counters are per-port statistics.
type Counters struct {
	RxPackets, TxPackets uint64
	RxBytes, TxBytes     uint64
	RxErrors, TxErrors   uint64
	RxDrops, TxDrops     uint64
}

// NextHop is one path of a route.
type NextHop struct {
	Via  netip.Addr
	Port string
}

// Route is a forwarding entry. More than one next-hop requires ECMP; an
// implementation without it must refuse rather than install the first.
type Route struct {
	Prefix   netip.Prefix
	NextHops []NextHop
}

// VLAN is one 802.1Q VLAN as the datapath holds it.
type VLAN struct {
	VID     int
	Members []VLANMember
	// SVI is whether the VLAN has a routed interface, named SVIName(VID).
	SVI bool
}

// VLANMember is one port's membership. Untagged is the port's native VLAN:
// frames it receives without a tag are placed in this VLAN, and frames it
// sends from it leave without one. That is an access port, or a trunk's
// native VLAN.
type VLANMember struct {
	Port   string
	Tagged bool
}

// SVIName is the interface a routed VLAN appears as. One spelling for every
// datapath, so that "iface vlan10 ..." in a configuration file means the same
// thing on every board.
func SVIName(vid int) string { return fmt.Sprintf("vlan%d", vid) }

// LAG is one link aggregation group, a port-channel.
type LAG struct {
	Name    string
	LACP    bool
	Members []LAGMember
}

// LAGMember is one port of a LAG. Active is whether it is carrying traffic
// now: link up and, for LACP, collecting and distributing -- a member that is
// configured but not active is the thing an operator needs to see.
type LAGMember struct {
	Port   string
	Active bool
}

// LAGName is the interface a LAG appears as: po1, po2 ... One spelling on
// every datapath, like SVIName.
func LAGName(n int) string { return fmt.Sprintf("po%d", n) }

// FDBEntry is one learned or static MAC.
type FDBEntry struct {
	MAC    string
	VLAN   int
	Port   string
	Static bool
}

// Switch is the contract. An implementation that cannot do something returns
// ErrUnsupported from that method and says so in Capabilities; the two must
// agree, and the conformance suite checks that they do.
type Switch interface {
	// Capabilities is what this datapath can do. It must be callable before
	// Start, so a caller can decide what to attempt.
	Capabilities() Capabilities

	// Start brings the datapath up; Close shuts it down.
	Start() error
	Close() error

	// Ports lists front-panel ports in board order.
	Ports() ([]Port, error)
	PortStatus(name string) (PortStatus, error)
	SetPortAdmin(name string, up bool) error
	SetPortMTU(name string, mtu int) error
	PortCounters(name string) (Counters, error)

	// VLANs.
	//
	// AddVLAN creates the VLAN; a datapath may refuse a VID it reserves for
	// its own use, and must say which. DelVLAN removes it and its
	// memberships, and refuses while it has an SVI.
	//
	// SetPortVLAN makes the port a member. Untagged also makes it the port's
	// native VLAN, replacing any earlier untagged membership: a port has one
	// native VLAN, and a second would make an untagged frame ambiguous. A
	// port with any membership is a switched port, and stops being a routed
	// one. DelPortVLAN removes one membership; removing the last returns the
	// port to routed.
	AddVLAN(vid int) error
	DelVLAN(vid int) error
	SetPortVLAN(name string, vid int, tagged bool) error
	DelPortVLAN(name string, vid int) error
	VLANs() ([]VLAN, error)

	// AddSVI gives an existing VLAN a routed interface, SVIName(vid), which
	// takes addresses through AddAddress like a port does. DelSVI removes it.
	AddSVI(vid int) error
	DelSVI(vid int) error

	// Link aggregation.
	//
	// AddLAG creates the LAG, static or LACP; an existing one keeps its
	// members and changes mode. SetLAGMembers states its whole membership:
	// ports it does not name leave, and are ordinary ports again. A member
	// must be neither switched nor in another LAG. DelLAG removes it and frees
	// its members. A LAG's name is accepted wherever a port name is -- in
	// SetPortVLAN, AddAddress, PortStatus -- except as another LAG's member.
	AddLAG(name string, lacp bool) error
	SetLAGMembers(name string, ports []string) error
	DelLAG(name string) error
	LAGs() ([]LAG, error)

	// Access lists. See acl.go. SetACL adds the rule or replaces the one with
	// the same sequence number; DelACL removes it; ACLs lists every rule the
	// datapath holds, installed or not, with its hit count.
	ACLs() ([]ACLEntry, error)
	SetACL(r ACLRule) error
	DelACL(seq int) error

	// L2.
	FDB() ([]FDBEntry, error)

	// L3.
	AddAddress(port string, addr netip.Prefix) error
	DelAddress(port string, addr netip.Prefix) error
	AddRoute(r Route) error
	DelRoute(prefix netip.Prefix) error
	Routes() ([]Route, error)
}
