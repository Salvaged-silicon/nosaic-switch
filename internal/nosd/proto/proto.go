// Package proto is the wire protocol between the nosaic CLI and nosd.
//
// One request and one response per connection, newline-delimited JSON over a
// Unix socket. Deliberately dull: this runs on switches with slow CPUs and
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
	OpCapabilities = "capabilities"
	OpPorts        = "ports"
	OpPortStatus   = "port.status"
	OpSetPortAdmin = "port.admin"
	OpSetPortMTU   = "port.mtu"
	OpPortCounters = "port.counters"
	OpAddVLAN      = "vlan.add"
	OpDelVLAN      = "vlan.del"
	OpSetPortVLAN  = "vlan.port"
	OpFDB          = "l2.fdb"
	OpAddAddress   = "l3.addr.add"
	OpDelAddress   = "l3.addr.del"
	OpAddRoute     = "l3.route.add"
	OpDelRoute     = "l3.route.del"
	OpRoutes       = "l3.routes"
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
