// Package client talks to nosd.
//
// It implements switchapi.Switch itself, which is the point: everything above
// the socket — the CLI, the config renderer, anything later — is written
// against the same interface whether the datapath is in this process or on the
// other end of a connection.
//
// It also means the conformance suite can be run *through* the socket. That is
// worth more than it sounds: a protocol can round-trip every value correctly
// and still lose the distinction between "this hardware cannot" and "this went
// wrong", and a caller that cannot tell those apart will retry something that
// will never work.
package client

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"net/netip"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/proto"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// Client is a switchapi.Switch backed by a nosd socket.
type Client struct {
	path string
	conn net.Conn
	enc  *json.Encoder
	dec  *json.Decoder
}

// Dial connects to nosd.
func Dial(path string) (*Client, error) {
	if path == "" {
		path = proto.SocketPath
	}
	conn, err := net.DialTimeout("unix", path, 5*time.Second)
	if err != nil {
		return nil, fmt.Errorf("cannot reach nosd at %s: %w "+
			"(is the datapath running?)", path, err)
	}
	return &Client{
		path: path,
		conn: conn,
		enc:  json.NewEncoder(conn),
		dec:  json.NewDecoder(bufio.NewReader(conn)),
	}, nil
}

func (c *Client) call(op string, args any, out any) error {
	var raw json.RawMessage
	if args != nil {
		b, err := json.Marshal(args)
		if err != nil {
			return err
		}
		raw = b
	}
	if err := c.enc.Encode(proto.Request{Op: op, Args: raw}); err != nil {
		return err
	}
	var resp proto.Response
	if err := c.dec.Decode(&resp); err != nil {
		return err
	}
	if err := resp.Err(); err != nil {
		return err
	}
	if out != nil && len(resp.Result) > 0 {
		return json.Unmarshal(resp.Result, out)
	}
	return nil
}

// Capabilities reports what the datapath can do.
//
// The interface has no error return here, so a failure to reach nosd surfaces
// as an empty capability set: everything unsupported, which is the safe
// reading. Callers that need to distinguish should Dial first.
func (c *Client) Capabilities() switchapi.Capabilities {
	var caps switchapi.Capabilities
	_ = c.call(proto.OpCapabilities, nil, &caps)
	return caps
}

// Start and Close are the client's own lifecycle: the datapath is already
// running, since something had to be listening for Dial to succeed.
func (c *Client) Start() error { return nil }
func (c *Client) Close() error { return c.conn.Close() }

func (c *Client) Ports() ([]switchapi.Port, error) {
	var ports []switchapi.Port
	return ports, c.call(proto.OpPorts, nil, &ports)
}

func (c *Client) PortStatus(name string) (switchapi.PortStatus, error) {
	var st switchapi.PortStatus
	return st, c.call(proto.OpPortStatus, proto.PortArgs{Name: name}, &st)
}

func (c *Client) SetPortAdmin(name string, up bool) error {
	return c.call(proto.OpSetPortAdmin, proto.PortArgs{Name: name, Up: up}, nil)
}

func (c *Client) SetPortMTU(name string, mtu int) error {
	return c.call(proto.OpSetPortMTU, proto.PortArgs{Name: name, MTU: mtu}, nil)
}

func (c *Client) PortCounters(name string) (switchapi.Counters, error) {
	var ctr switchapi.Counters
	return ctr, c.call(proto.OpPortCounters, proto.PortArgs{Name: name}, &ctr)
}

func (c *Client) AddVLAN(vid int) error {
	return c.call(proto.OpAddVLAN, proto.VLANArgs{VID: vid}, nil)
}

func (c *Client) DelVLAN(vid int) error {
	return c.call(proto.OpDelVLAN, proto.VLANArgs{VID: vid}, nil)
}

func (c *Client) SetPortVLAN(name string, vid int, tagged bool) error {
	return c.call(proto.OpSetPortVLAN, proto.VLANArgs{Port: name, VID: vid, Tagged: tagged}, nil)
}

func (c *Client) FDB() ([]switchapi.FDBEntry, error) {
	var f []switchapi.FDBEntry
	return f, c.call(proto.OpFDB, nil, &f)
}

func (c *Client) AddAddress(port string, addr netip.Prefix) error {
	return c.call(proto.OpAddAddress, proto.AddrArgs{Port: port, Prefix: addr.String()}, nil)
}

func (c *Client) DelAddress(port string, addr netip.Prefix) error {
	return c.call(proto.OpDelAddress, proto.AddrArgs{Port: port, Prefix: addr.String()}, nil)
}

func (c *Client) AddRoute(r switchapi.Route) error {
	args := proto.RouteArgs{Prefix: r.Prefix.String()}
	for _, nh := range r.NextHops {
		args.NextHops = append(args.NextHops, proto.NextHopArgs{Via: nh.Via.String(), Port: nh.Port})
	}
	return c.call(proto.OpAddRoute, args, nil)
}

func (c *Client) DelRoute(p netip.Prefix) error {
	return c.call(proto.OpDelRoute, proto.RouteArgs{Prefix: p.String()}, nil)
}

func (c *Client) Routes() ([]switchapi.Route, error) {
	var raw []proto.RouteArgs
	if err := c.call(proto.OpRoutes, nil, &raw); err != nil {
		return nil, err
	}
	out := make([]switchapi.Route, 0, len(raw))
	for _, r := range raw {
		pfx, err := netip.ParsePrefix(r.Prefix)
		if err != nil {
			return nil, err
		}
		route := switchapi.Route{Prefix: pfx}
		for _, nh := range r.NextHops {
			via, err := netip.ParseAddr(nh.Via)
			if err != nil {
				return nil, err
			}
			route.NextHops = append(route.NextHops, switchapi.NextHop{Via: via, Port: nh.Port})
		}
		out = append(out, route)
	}
	return out, nil
}
