// Package server exposes a switchapi.Switch over a Unix socket.
//
// nosd owns the chip; everything else talks to it through here. Keeping the
// datapath in one process matters on real hardware, where a vendor SDK holds
// state that cannot sensibly be shared between processes — but the reason it
// is a socket rather than a library is that the CLI must work identically
// whether the datapath is a Broadcom SDK, an in-kernel driver, or veth pairs.
package server

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"path/filepath"

	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/proto"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// Server serves one switch.
type Server struct {
	sw  switchapi.Switch
	log io.Writer
}

// New builds a server around a datapath.
func New(sw switchapi.Switch, log io.Writer) *Server {
	if log == nil {
		log = io.Discard
	}
	return &Server{sw: sw, log: log}
}

// Listen serves until the listener is closed.
func (s *Server) Listen(path string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	// A socket left behind by a killed daemon would otherwise make every
	// restart fail with "address already in use", which on a switch means a
	// datapath that will not come back after a crash.
	_ = os.Remove(path)

	ln, err := net.Listen("unix", path)
	if err != nil {
		return err
	}
	defer ln.Close()
	// Root-only: this socket configures forwarding.
	if err := os.Chmod(path, 0o600); err != nil {
		return err
	}
	fmt.Fprintf(s.log, "nosd: listening on %s (driver %s)\n", path, s.sw.Capabilities().Driver)

	for {
		conn, err := ln.Accept()
		if err != nil {
			return err
		}
		go s.serve(conn)
	}
}

func (s *Server) serve(conn net.Conn) {
	defer conn.Close()
	dec := json.NewDecoder(bufio.NewReader(conn))
	enc := json.NewEncoder(conn)

	for {
		var req proto.Request
		if err := dec.Decode(&req); err != nil {
			return
		}
		resp := s.dispatch(req)
		if err := enc.Encode(resp); err != nil {
			return
		}
	}
}

func (s *Server) dispatch(req proto.Request) proto.Response {
	ok := func(v any) proto.Response {
		b, err := json.Marshal(v)
		if err != nil {
			return proto.ErrorResponse(err)
		}
		return proto.Response{OK: true, Result: b}
	}
	done := func(err error) proto.Response {
		if err != nil {
			return proto.ErrorResponse(err)
		}
		return proto.Response{OK: true}
	}
	var p proto.PortArgs
	var v proto.VLANArgs
	var a proto.AddrArgs
	var r proto.RouteArgs

	switch req.Op {
	case proto.OpCapabilities:
		return ok(s.sw.Capabilities())

	case proto.OpPorts:
		ports, err := s.sw.Ports()
		if err != nil {
			return proto.ErrorResponse(err)
		}
		return ok(ports)

	case proto.OpPortStatus:
		if err := json.Unmarshal(req.Args, &p); err != nil {
			return proto.ErrorResponse(err)
		}
		st, err := s.sw.PortStatus(p.Name)
		if err != nil {
			return proto.ErrorResponse(err)
		}
		return ok(st)

	case proto.OpSetPortAdmin:
		if err := json.Unmarshal(req.Args, &p); err != nil {
			return proto.ErrorResponse(err)
		}
		return done(s.sw.SetPortAdmin(p.Name, p.Up))

	case proto.OpSetPortMTU:
		if err := json.Unmarshal(req.Args, &p); err != nil {
			return proto.ErrorResponse(err)
		}
		return done(s.sw.SetPortMTU(p.Name, p.MTU))

	case proto.OpPortCounters:
		if err := json.Unmarshal(req.Args, &p); err != nil {
			return proto.ErrorResponse(err)
		}
		c, err := s.sw.PortCounters(p.Name)
		if err != nil {
			return proto.ErrorResponse(err)
		}
		return ok(c)

	case proto.OpAddVLAN:
		if err := json.Unmarshal(req.Args, &v); err != nil {
			return proto.ErrorResponse(err)
		}
		return done(s.sw.AddVLAN(v.VID))

	case proto.OpDelVLAN:
		if err := json.Unmarshal(req.Args, &v); err != nil {
			return proto.ErrorResponse(err)
		}
		return done(s.sw.DelVLAN(v.VID))

	case proto.OpSetPortVLAN:
		if err := json.Unmarshal(req.Args, &v); err != nil {
			return proto.ErrorResponse(err)
		}
		return done(s.sw.SetPortVLAN(v.Port, v.VID, v.Tagged))

	case proto.OpFDB:
		f, err := s.sw.FDB()
		if err != nil {
			return proto.ErrorResponse(err)
		}
		return ok(f)

	case proto.OpAddAddress, proto.OpDelAddress:
		if err := json.Unmarshal(req.Args, &a); err != nil {
			return proto.ErrorResponse(err)
		}
		pfx, err := netip.ParsePrefix(a.Prefix)
		if err != nil {
			return proto.ErrorResponse(err)
		}
		if req.Op == proto.OpAddAddress {
			return done(s.sw.AddAddress(a.Port, pfx))
		}
		return done(s.sw.DelAddress(a.Port, pfx))

	case proto.OpAddRoute:
		if err := json.Unmarshal(req.Args, &r); err != nil {
			return proto.ErrorResponse(err)
		}
		route, err := decodeRoute(r)
		if err != nil {
			return proto.ErrorResponse(err)
		}
		return done(s.sw.AddRoute(route))

	case proto.OpDelRoute:
		if err := json.Unmarshal(req.Args, &r); err != nil {
			return proto.ErrorResponse(err)
		}
		pfx, err := netip.ParsePrefix(r.Prefix)
		if err != nil {
			return proto.ErrorResponse(err)
		}
		return done(s.sw.DelRoute(pfx))

	case proto.OpRoutes:
		routes, err := s.sw.Routes()
		if err != nil {
			return proto.ErrorResponse(err)
		}
		return ok(encodeRoutes(routes))
	}
	return proto.ErrorResponse(fmt.Errorf("unknown operation %q", req.Op))
}

func decodeRoute(r proto.RouteArgs) (switchapi.Route, error) {
	pfx, err := netip.ParsePrefix(r.Prefix)
	if err != nil {
		return switchapi.Route{}, err
	}
	out := switchapi.Route{Prefix: pfx}
	for _, nh := range r.NextHops {
		via, err := netip.ParseAddr(nh.Via)
		if err != nil {
			return switchapi.Route{}, err
		}
		out.NextHops = append(out.NextHops, switchapi.NextHop{Via: via, Port: nh.Port})
	}
	return out, nil
}

func encodeRoutes(routes []switchapi.Route) []proto.RouteArgs {
	out := make([]proto.RouteArgs, 0, len(routes))
	for _, r := range routes {
		ra := proto.RouteArgs{Prefix: r.Prefix.String()}
		for _, nh := range r.NextHops {
			ra.NextHops = append(ra.NextHops, proto.NextHopArgs{Via: nh.Via.String(), Port: nh.Port})
		}
		out = append(out, ra)
	}
	return out
}
