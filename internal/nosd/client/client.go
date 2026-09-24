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

// ASICTemperature is one of the switch die's own temperature monitors.
//
// Millidegrees, like hwmon and like the platform HAL, converted from the
// SDK's 0.1 C by the datapath so there is exactly one place that knows.
type ASICTemperature struct {
	Index      int
	MilliC     int
	PeakMilliC int
}

// ASICTemperatures reads the switch die.
//
// Not part of the switchapi contract, for the same reason DMAPool is not: a
// die sensor is a property of a datapath driving real silicon. It lives here
// rather than in a board's HAL because the only path to it is the SDK over
// PCIe -- no i2c part on any of these boards can see the die, which is why
// every board's cooling loop has so far been regulating on sensors sited
// near the ASIC rather than on it.
//
// An empty slice with no error means the chip has no monitors. That is
// different from a failure, and a cooling loop has to tell them apart.
func (c *Client) ASICTemperatures() ([]ASICTemperature, error) {
	var out []ASICTemperature
	err := c.call("asic.temp", nil, &out)
	return out, err
}

// PHYRegs is one external PHY's status registers, read straight off the MDIO
// bus by address rather than through whatever driver the SDK bound.
//
// Regs is left as the raw register name to value map the datapath sent, with
// a nil value for a read that failed. Nothing is decoded here: which part
// answers depends on the board, and a decode that assumes the wrong one is
// worse than the number.
type PHYRegs struct {
	Port   int
	Addr   uint16
	Driver string
	Regs   map[string]*uint16
}

// PHYs reads every external PHY's status registers.
//
// A diagnostic, like DMAPool: it exists because a 40G cage that transmits
// correctly and never receives looks exactly like a bad fibre from anything
// the switch API reports, and telling those apart means asking the PHY which
// layer is unhappy.
func (c *Client) PHYs() ([]PHYRegs, error) {
	var out []PHYRegs
	err := c.call("phy.dump", nil, &out)
	return out, err
}

// PHYReg is one register read: Value is nil when the read itself failed,
// which is a different answer from a register that holds zero.
type PHYReg struct {
	Reg   int
	Value *uint16
	// ViaDriver says which path answered. False means the raw MIIM bus by
	// address, which on a BCM84328 returns 0 for signal detect even on a
	// linked port -- see the note in datapath/td2/phy.c. A raw answer is
	// not necessarily wrong, but it is not to be trusted on its own.
	ViaDriver bool
}

// PHYRead reads count consecutive registers from one port's external PHY.
//
// devad is the Clause 45 MMD. Takes a range because the useful questions of a
// part that is only half awake -- which MMDs it implements, what it calls
// itself, whether its firmware is running -- are not known one at a time.
func (c *Client) PHYRead(port, devad, reg, count int) ([]PHYReg, error) {
	var out []PHYReg
	err := c.call("phy.read", map[string]int{
		"port": port, "devad": devad, "reg": reg, "count": count,
	}, &out)
	return out, err
}

// PHYWrite is the result of writing one register: Value is the read-back, so
// a register that ignored the write shows as a Value that is not Wrote.
type PHYWrite struct {
	Reg   int
	Wrote int
	Value *uint16
	Ok    bool
}

// PHYWriteReg writes one register of a port's external PHY and reads it back.
//
// A bring-up tool: it can take a working port down. It exists because some
// questions have no read-only form -- whether a far end's link actually
// depends on our transmitter, for one, which needs our transmitter turned off.
func (c *Client) PHYWriteReg(port, devad, reg, val int) ([]PHYWrite, error) {
	var out []PHYWrite
	err := c.call("phy.write", map[string]int{
		"port": port, "devad": devad, "reg": reg, "value": val,
	}, &out)
	return out, err
}

// Loopback is one port's loopback mode, as the chip reports it back.
type Loopback struct {
	Port int
	Mode int
}

// SetLoopback puts a port into one of the chip's loopbacks, or with mode < 0
// only reads the current one. Modes are the SDK's: 0 none, 1 MAC, 2 PHY,
// 3 PHY remote, 4 MAC remote, 5 EDB.
//
// A bring-up tool: it breaks traffic on the port. It exists because a port
// that transmits correctly and never receives looks the same from every API
// the switch offers, and a loopback is what splits that path in two.
func (c *Client) SetLoopback(port, mode int) (Loopback, error) {
	var out Loopback
	err := c.call("port.loopback", map[string]int{"port": port, "mode": mode}, &out)
	return out, err
}

// SetDeadline bounds every subsequent call on this connection.
//
// ⚠ THERE IS NO DEFAULT DEADLINE, AND ONE CALLER CANNOT AFFORD THAT.
//
// call() writes a request and blocks reading the reply. A nosd that is alive
// enough to accept the connection but wedged before answering leaves the
// caller blocked for ever. For the CLI that is a hang somebody can Ctrl-C;
// for the cooling loop, which asks this daemon for the die temperature every
// interval, it is fans frozen at their last duty with nothing logged.
func (c *Client) SetDeadline(t time.Time) error { return c.conn.SetDeadline(t) }

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

func (c *Client) DelPortVLAN(name string, vid int) error {
	return c.call(proto.OpDelPortVLAN, proto.VLANArgs{Port: name, VID: vid}, nil)
}

func (c *Client) VLANs() ([]switchapi.VLAN, error) {
	var vl []switchapi.VLAN
	return vl, c.call(proto.OpVLANs, nil, &vl)
}

func (c *Client) AddSVI(vid int) error {
	return c.call(proto.OpAddSVI, proto.VLANArgs{VID: vid}, nil)
}

func (c *Client) DelSVI(vid int) error {
	return c.call(proto.OpDelSVI, proto.VLANArgs{VID: vid}, nil)
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

// ACLList is the acl operation's whole answer: the rules and each family's
// room. ACLs, the contract method, is the rules alone; the room is on
// Capabilities, and this exists for `show acl` to print both from one call.
func (c *Client) ACLList() (proto.ACLList, error) {
	var out proto.ACLList
	err := c.call(proto.OpACL, nil, &out)
	return out, err
}

func (c *Client) ACLs() ([]switchapi.ACLEntry, error) {
	l, err := c.ACLList()
	if err != nil {
		return nil, err
	}
	out := make([]switchapi.ACLEntry, 0, len(l.Rules))
	for _, w := range l.Rules {
		e := switchapi.ACLEntry{
			Seq: w.Seq, Text: w.Rule, Installed: w.Installed,
			Packets: w.Packets, Error: w.Error,
		}
		if w.Parsed {
			if r, perr := switchapi.ParseACLRule(w.Seq, w.Rule); perr == nil {
				e.Rule = r
				e.Parsed = true
			}
		}
		out = append(out, e)
	}
	return out, nil
}

// SetACL sends the rule in its text form; the far side parses it with the
// same grammar, so what is installed is what String says.
func (c *Client) SetACL(r switchapi.ACLRule) error {
	return c.call(proto.OpSetACL, proto.ACLSetArgs{Seq: r.Seq, Rule: r.String()}, nil)
}

func (c *Client) DelACL(seq int) error {
	return c.call(proto.OpDelACL, proto.ACLDelArgs{Seq: seq}, nil)
}
