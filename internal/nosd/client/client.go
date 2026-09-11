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
	"errors"
	"fmt"
	"net"
	"net/netip"
	"os"
	"syscall"
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
		return nil, dialError(path, err)
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
// DMAPool is what the datapath's DMA allocator holds, and who is holding it.
//
// Not part of the switchapi contract: a DMA pool is a property of a datapath
// that drives silicon through a vendor SDK, and the virtual platform has
// nothing to report. Asking a datapath without one is answered with a refusal
// rather than with zeroes, which is the capability model doing its job.
type DMAPool struct {
	Bytes, Used, Largest, Peak uint64
	Fails                      uint64
	Callers                    []DMACaller
}

// DMACaller is one allocation name's share of the pool. Outstanding climbing
// with uptime is a leak, and naming the caller is the whole point: the first
// time this pool was exhausted, working out which caller had taken it meant
// reading the vendor's source and inferring.
type DMACaller struct {
	Name                 string
	Outstanding, Peak    uint64
	Allocs, Frees, Fails uint64
}

func (c *Client) DMAPool() (DMAPool, error) {
	var out DMAPool
	err := c.call("asic.dma", nil, &out)
	return out, err
}

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

// dialError says why the socket could not be opened.
//
// It used to be one message -- "is the datapath running?" -- for every cause.
// That describes a missing socket well and a permission failure not at all,
// and the socket is mode 0600 owned by root, so the ordinary case of an
// operator running `nosaic show ports` as the login account got a message
// about the datapath being down. The datapath was fine. Pointing the next
// person at the silicon instead of at the privilege path is a real cost, and
// the errno needed to tell them apart was already in hand.
func dialError(path string, err error) error {
	switch {
	case errors.Is(err, os.ErrPermission):
		return fmt.Errorf("not allowed to open %s: %w\n"+
			"The datapath is probably fine: this socket is root-only, and you "+
			"are not root. Try `doas nosaic ...` (or `sudo nosaic ...`).", path, err)
	case errors.Is(err, os.ErrNotExist):
		return fmt.Errorf("there is no %s: %w\n"+
			"The daemon creates it once the chip is up; if nosd is running and "+
			"this is missing, it did not get that far.", path, err)
	case errors.Is(err, syscall.ECONNREFUSED):
		return fmt.Errorf("%s exists but nothing is listening on it: %w\n"+
			"That is what a daemon that died without tidying up leaves behind.", path, err)
	default:
		return fmt.Errorf("cannot reach nosd at %s: %w", path, err)
	}
}
