// Package proto is the wire protocol between the nosaic CLI and nosd.
//
// Newline-delimited JSON over a Unix socket: one response per request, and as
// many requests per connection as the client sends. The CLI dials once and
// issues every call down the same socket, so a server that answers one and
// closes breaks the second call with a write error that looks like a network
// fault. This said "one request and one response per connection", which is what
// a second implementation of it then got wrong. Deliberately dull: this runs on switches with slow CPUs and
// small memories, it has to be debuggable with the tools present in a minimal
// image, and a protocol you can read with cat is worth more here than one that
// is fast.
//
// The socket is the boundary between "what a user asked for" and "how this
// chip does it". Everything above it is identical on every switch.
package proto

import (
	"encoding/json"
	"errors"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// SocketPath is where nosd listens. Under /run because it is runtime state:
// it belongs on a tmpfs that is empty again after a reboot, not in the image
// and not on the data partition.
const SocketPath = "/run/nosd.sock"

// Request is one call.
type Request struct {
	Op   string          `json:"op"`
	Args json.RawMessage `json:"args,omitempty"`
}

// Response is its result.
//
// Error carries the message and Unsupported records whether it was an
// ErrUnsupported. Without that second field the distinction between "this
// hardware cannot" and "this went wrong" would not survive the wire, and the
// capability model would quietly stop meaning anything at the socket.
type Response struct {
	OK          bool            `json:"ok"`
	Error       string          `json:"error,omitempty"`
	Unsupported bool            `json:"unsupported,omitempty"`
	Result      json.RawMessage `json:"result,omitempty"`
}

// ErrorResponse builds a failure response, preserving whether the cause was an
// unsupported operation.
func ErrorResponse(err error) Response {
	return Response{
		OK:          false,
		Error:       err.Error(),
		Unsupported: errors.Is(err, switchapi.ErrUnsupported),
	}
}

// Err reconstructs an error from a response, preserving whether it was an
// unsupported operation so that errors.Is keeps working on the client side.
func (r Response) Err() error {
	if r.OK {
		return nil
	}
	return remoteError{msg: r.Error, unsupported: r.Unsupported}
}

// remoteError carries a message from the far side and, separately, whether it
// was an ErrUnsupported.
//
// A wrapper type rather than fmt.Errorf("%s: %w", ...): the message has already
// travelled with the sentinel's text in it, so re-wrapping printed "not
// supported by this datapath: not supported by this datapath". Unwrap keeps
// errors.Is working without touching the message at all.
type remoteError struct {
	msg         string
	unsupported bool
}

func (e remoteError) Error() string { return e.msg }

func (e remoteError) Unwrap() error {
	if e.unsupported {
		return switchapi.ErrUnsupported
	}
	return nil
}

// Operation names. Strings rather than numbers so a capture is readable and a
// mismatched client says what it asked for.
const (
	OpCapabilities  = "capabilities"
	OpPorts         = "ports"
	OpPortStatus    = "port.status"
	OpSetPortAdmin  = "port.admin"
	OpSetPortMTU    = "port.mtu"
	OpPortCounters  = "port.counters"
	OpAddVLAN       = "vlan.add"
	OpDelVLAN       = "vlan.del"
	OpSetPortVLAN   = "vlan.port"
	OpDelPortVLAN   = "vlan.port.del"
	OpVLANs         = "vlans"
	OpAddSVI        = "svi.add"
	OpDelSVI        = "svi.del"
	OpAddLAG        = "lag.add"
	OpSetLAGMembers = "lag.members"
	OpDelLAG        = "lag.del"
	OpLAGs          = "lags"
	OpSetSTP        = "stp.set"
	OpSetSTPPort    = "stp.port"
	OpSTP           = "stp"
	OpSetMLAG       = "mlag.set"
	OpSetLAGOptions = "lag.options"
	OpSetLACPPrio   = "lacp.priority"
	OpSetLAGMLAG    = "lag.mlag"
	OpMLAG          = "mlag"
	OpSetVirtualMAC = "gateway.mac"
	OpAddGateway    = "gateway.add"
	OpDelGateway    = "gateway.del"
	OpGateways      = "gateways"
	OpFDB           = "l2.fdb"
	OpAddAddress    = "l3.addr.add"
	OpDelAddress    = "l3.addr.del"
	OpAddRoute      = "l3.route.add"
	OpDelRoute      = "l3.route.del"
	OpRoutes        = "l3.routes"
	OpACL           = "acl"
	OpSetACL        = "acl.set"
	OpDelACL        = "acl.del"
)

// Argument shapes.

type PortArgs struct {
	Name string `json:"name"`
	Up   bool   `json:"up,omitempty"`
	MTU  int    `json:"mtu,omitempty"`
}

type VLANArgs struct {
	VID    int    `json:"vid"`
	Port   string `json:"port,omitempty"`
	Tagged bool   `json:"tagged,omitempty"`
}

// LAGArgs is a LAG by name; Ports is its whole membership for lag.members.
type LAGArgs struct {
	Name  string   `json:"name"`
	LACP  bool     `json:"lacp,omitempty"`
	Ports []string `json:"ports,omitempty"`
}

// STPArgs sets the bridge. No omitempty: false and priority 0 are both
// meaningful, and a datapath that parses by key must find them.
type STPArgs struct {
	Enabled      bool `json:"enabled"`
	Priority     int  `json:"priority"`
	HelloTime    int  `json:"hello_time"`
	ForwardDelay int  `json:"forward_delay"`
	MaxAge       int  `json:"max_age"`
}

// STPPortArgs configures one interface. Cost 0 is the speed's default.
type STPPortArgs struct {
	Name     string `json:"name"`
	Edge     bool   `json:"edge"`
	Cost     int    `json:"cost"`
	Priority int    `json:"port_priority"`
}

// MLAGArgs sets the pair; no omitempty, for the same reason as STPArgs.
type MLAGArgs struct {
	Enabled       bool   `json:"enabled"`
	PeerLink      string `json:"peer_link"`
	PeerAddress   string `json:"peer_address"`
	Priority      int    `json:"priority"`
	HelloMs       int    `json:"hello_ms"`
	DeadMs        int    `json:"dead_ms"`
	SettleMs      int    `json:"settle_ms"`
	HeartbeatPort int    `json:"heartbeat_port"`
}

// LAGOptionsArgs tunes a LAG's LACP.
type LAGOptionsArgs struct {
	Name         string `json:"name"`
	Rate         string `json:"rate"`
	Passive      bool   `json:"passive"`
	PortPriority int    `json:"port_priority"`
}

// PriorityArgs carries one priority.
type PriorityArgs struct {
	Priority int `json:"priority"`
}

// LAGMLAGArgs gives a LAG its MLAG id, or 0 to take it back.
type LAGMLAGArgs struct {
	Name string `json:"name"`
	ID   int    `json:"mlag"`
}

// GatewayArgs names an SVI and an address, or carries the virtual MAC.
type GatewayArgs struct {
	SVI    string `json:"svi,omitempty"`
	Prefix string `json:"prefix,omitempty"`
	MAC    string `json:"mac,omitempty"`
}

type AddrArgs struct {
	Port   string `json:"port"`
	Prefix string `json:"prefix"`
}

type RouteArgs struct {
	Prefix   string        `json:"prefix"`
	NextHops []NextHopArgs `json:"nexthops,omitempty"`
}

type NextHopArgs struct {
	Via  string `json:"via"`
	Port string `json:"port"`
}

// ACLSetArgs carries a rule in its text form. Text rather than fields,
// because the text is the form operators type and configuration files hold,
// and every datapath already parses it for itself; a second, structured
// encoding would be a second grammar to keep in step.
type ACLSetArgs struct {
	Seq  int    `json:"seq"`
	Rule string `json:"rule"`
}

type ACLDelArgs struct {
	Seq int `json:"seq"`
}

// ACLEntry is one rule as listed. Rule is the canonical text when Parsed,
// and the text as configured when not.
type ACLEntry struct {
	Seq       int    `json:"Seq"`
	Family    int    `json:"Family"`
	Rule      string `json:"Rule"`
	Parsed    bool   `json:"Parsed"`
	Installed bool   `json:"Installed"`
	Packets   uint64 `json:"Packets"`
	Error     string `json:"Error"`
}

// ACLList is the acl operation's result: the rules and the room each family
// has, which is what tells an operator sizing a list whether it will fit.
type ACLList struct {
	Available  bool       `json:"Available"`
	Total      int        `json:"Total"`
	Free       int        `json:"Free"`
	Available6 bool       `json:"Available6"`
	Total6     int        `json:"Total6"`
	Free6      int        `json:"Free6"`
	Rules      []ACLEntry `json:"Rules"`
}
