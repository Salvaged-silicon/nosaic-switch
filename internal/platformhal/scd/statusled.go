package scd

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// The chassis status lamps: system status, the two power supplies, and fans.
//
// These are NOT on the SCD, which is where every other lamp on this board
// lives. They are on the same CPLD as the fan controller, reached over the
// SCD's SMBus master -- the port LEDs are memory-mapped and these are behind a
// bus. One panel, two entirely different paths to light it.
//
// # Why the register map is not in this file
//
// The offsets and bit positions are vendor board-description data, read off a
// machine running the vendor's OS. They are the vendor's numbers and are not
// ours to publish, so NOSaic ships the generator and not its output -- the same
// arrangement as tools/mkportmap.sh and tools/mkpolarity.sh, and for the same
// reason. Generate the file once against your own switch with
// platform/arista-7050sx2-72q/tools/mkstatusleds.sh and drop it in that board's
// config/, where the image builder copies it to /etc/nosaic.
//
// Without it the lamps are simply unsupported, and the CLI says so. A dark
// panel is a cosmetic fault; a panel driven from a guessed map would be a
// switch telling an operator something false about its own health.
//
// ⚠ Do not go looking for these by sweeping the CPLD. Two attempts to find the
// fan PWM register that way powered this switch off -- the note on crowAddr in
// thermal.go is about the same part.
const lampConfPath = "/etc/nosaic/statusleds.conf"

// ErrNoLampMap is returned when the board's lamp map has not been generated.
var ErrNoLampMap = errors.New(
	"this board's status-lamp map is not configured: generate it with " +
		"platform/arista-7050sx2-72q/tools/mkstatusleds.sh and install it as " +
		lampConfPath)

// Lamp is one chassis status light.
type Lamp struct {
	Name string
	Reg  int
}

// Colour is what a chassis lamp shows.
type Colour byte

const (
	LampOff Colour = iota
	LampRed
	LampGreen
	// LampAmber is both emitters lit. The panel shows amber, not a green and a
	// red one beside each other -- they are one lamp with two dies.
	LampAmber
)

func (c Colour) String() string {
	switch c {
	case LampOff:
		return "off"
	case LampRed:
		return "red"
	case LampGreen:
		return "green"
	case LampAmber:
		return "amber"
	}
	return fmt.Sprintf("unknown(%d)", byte(c))
}

// lampMap is the generated board data: which register is which lamp, and which
// bit in it is which colour.
type lampMap struct {
	order     []Lamp // health lamps, in panel order; the beacon is not among them
	beacon    int    // register of the blue locator, -1 if the board has none
	green     uint
	red       uint
	blue      uint
	loaded    bool
	loadErr   error
	attempted bool
}

// bits renders a colour into the two colour bits this board uses.
func (m *lampMap) bits(c Colour) byte {
	var v byte
	if c == LampGreen || c == LampAmber {
		v |= 1 << m.green
	}
	if c == LampRed || c == LampAmber {
		v |= 1 << m.red
	}
	return v
}

// colour reads the two colour bits back out of a register value.
func (m *lampMap) colour(v byte) Colour {
	c := LampOff
	if v&(1<<m.green) != 0 {
		c = LampGreen
	}
	if v&(1<<m.red) != 0 {
		if c == LampGreen {
			c = LampAmber
		} else {
			c = LampRed
		}
	}
	return c
}

// mask is every bit this driver owns in a lamp register. Anything else in there
// is left alone -- see SetLamp.
func (m *lampMap) mask() byte { return (1 << m.green) | (1 << m.red) }

/*
loadLamps reads the generated map.

The file is key=value, one per line, '#' comments:

	status    = 0xNN        # the system status lamp
	beacon    = 0xNN        # the blue locator, a capability OF status
	psu1      = 0xNN
	psu2      = 0xNN
	fan       = 0xNN
	green_bit = N
	red_bit   = N
	blue_bit  = N

The actual values are not written out here, in a comment or anywhere else in
this tree: they are the vendor's board data, and the generator is what produces
them.

Lamps that are absent from the file are absent from the panel, which is how a
board with fewer lamps is described rather than special-cased.
*/
func (s *SCD) loadLamps() (*lampMap, error) {
	if s.lamps == nil {
		s.lamps = &lampMap{beacon: -1}
	}
	m := s.lamps
	if m.attempted {
		return m, m.loadErr
	}
	m.attempted = true

	path := lampConfPath
	if p := os.Getenv("NOSAIC_STATUSLEDS"); p != "" {
		path = p
	}
	f, err := os.Open(path)
	if err != nil {
		m.loadErr = ErrNoLampMap
		return m, m.loadErr
	}
	defer f.Close()

	// -1 rather than 0, because 0 is a legal register offset and "unset" has to
	// be distinguishable from it.
	reg := map[string]int{"status": -1, "psu1": -1, "psu2": -1, "fan": -1, "beacon": -1}
	bit := map[string]int{"green_bit": -1, "red_bit": -1, "blue_bit": 0}

	sc := bufio.NewScanner(f)
	for ln := 1; sc.Scan(); ln++ {
		line := sc.Text()
		if i := strings.IndexByte(line, '#'); i >= 0 {
			line = line[:i]
		}
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			m.loadErr = fmt.Errorf("%s:%d: not key=value: %q", path, ln, line)
			return m, m.loadErr
		}
		k, v = strings.TrimSpace(k), strings.TrimSpace(v)
		n, perr := strconv.ParseUint(v, 0, 16)
		if perr != nil {
			m.loadErr = fmt.Errorf("%s:%d: %s: %w", path, ln, k, perr)
			return m, m.loadErr
		}
		switch k {
		case "status", "psu1", "psu2", "fan", "beacon":
			reg[k] = int(n)
		case "green_bit", "red_bit", "blue_bit":
			bit[k] = int(n)
		default:
			// Refused rather than ignored. A typo in a generated file would
			// otherwise leave a lamp silently undriven, which looks exactly
			// like a board that has no such lamp.
			m.loadErr = fmt.Errorf("%s:%d: unknown key %q", path, ln, k)
			return m, m.loadErr
		}
	}
	if err := sc.Err(); err != nil {
		m.loadErr = fmt.Errorf("%s: %w", path, err)
		return m, m.loadErr
	}

	// A map without the colour bits cannot light anything, and guessing them is
	// exactly the failure this whole arrangement exists to avoid.
	if bit["green_bit"] < 0 || bit["red_bit"] < 0 {
		m.loadErr = fmt.Errorf("%s: green_bit and red_bit are required", path)
		return m, m.loadErr
	}
	m.green, m.red, m.blue = uint(bit["green_bit"]), uint(bit["red_bit"]), uint(bit["blue_bit"])
	m.beacon = reg["beacon"]
	for _, n := range []string{"status", "psu1", "psu2", "fan"} {
		if reg[n] >= 0 {
			m.order = append(m.order, Lamp{n, reg[n]})
		}
	}
	if len(m.order) == 0 && m.beacon < 0 {
		m.loadErr = fmt.Errorf("%s: names no lamps", path)
		return m, m.loadErr
	}
	m.loaded = true
	return m, nil
}

// SetLamp lights one chassis lamp.
//
// Read-modify-write rather than a plain store: only the two colour bits belong
// to us, and what the other six do on this register is not established. On a
// part where a blind sweep has twice powered the switch off, writing bits
// nobody has accounted for is not a risk worth taking to save one read.
func (s *SCD) SetLamp(l Lamp, c Colour) error {
	m, err := s.loadLamps()
	if err != nil {
		return err
	}
	v, err := s.smb().ReadReg(crowAccel, crowBus, crowAddr, l.Reg)
	if err != nil {
		return fmt.Errorf("status lamp %s (CPLD %#02x reg %#02x): %w",
			l.Name, crowAddr, l.Reg, err)
	}
	v = (v &^ m.mask()) | m.bits(c)
	if err := s.smb().WriteReg(crowAccel, crowBus, crowAddr, l.Reg, v); err != nil {
		return fmt.Errorf("status lamp %s (CPLD %#02x reg %#02x): %w",
			l.Name, crowAddr, l.Reg, err)
	}
	return nil
}

// Lamps reports what the chassis lamps are currently showing.
func (s *SCD) Lamps() (map[string]Colour, error) {
	m, err := s.loadLamps()
	if err != nil {
		return nil, err
	}
	out := map[string]Colour{}
	var firstErr error
	for _, l := range m.order {
		v, err := s.smb().ReadReg(crowAccel, crowBus, crowAddr, l.Reg)
		if err != nil {
			if firstErr == nil {
				firstErr = fmt.Errorf("status lamp %s: %w", l.Name, err)
			}
			continue
		}
		out[l.Name] = m.colour(v)
	}
	if len(out) == 0 {
		return out, firstErr
	}
	return out, nil
}

// SetBeacon lights or clears the blue locator.
//
// It shares a lamp with the system status light and the blue wins: writing
// green to the status register with the beacon set shows blue. That is correct
// for a locator and it is why the health policy never writes this register. An
// operator who lit the beacon to find this box in a rack must not have a
// background loop switch it off underneath them -- they are standing in the
// aisle looking for it.
func (s *SCD) SetBeacon(on bool) error {
	m, err := s.loadLamps()
	if err != nil {
		return err
	}
	if m.beacon < 0 {
		return errors.New("this board has no locator beacon")
	}
	v, err := s.smb().ReadReg(crowAccel, crowBus, crowAddr, m.beacon)
	if err != nil {
		return fmt.Errorf("beacon (CPLD %#02x reg %#02x): %w", crowAddr, m.beacon, err)
	}
	if on {
		v |= 1 << m.blue
	} else {
		v &^= 1 << m.blue
	}
	if err := s.smb().WriteReg(crowAccel, crowBus, crowAddr, m.beacon, v); err != nil {
		return fmt.Errorf("beacon (CPLD %#02x reg %#02x): %w", crowAddr, m.beacon, err)
	}
	return nil
}

// BeaconOn reports whether the blue locator is lit.
func (s *SCD) BeaconOn() (bool, error) {
	m, err := s.loadLamps()
	if err != nil {
		return false, err
	}
	if m.beacon < 0 {
		return false, errors.New("this board has no locator beacon")
	}
	v, err := s.smb().ReadReg(crowAccel, crowBus, crowAddr, m.beacon)
	if err != nil {
		return false, fmt.Errorf("beacon (CPLD %#02x reg %#02x): %w", crowAddr, m.beacon, err)
	}
	return v&(1<<m.blue) != 0, nil
}

// LampSummary is Lamps rendered for a human, plus the beacon.
//
// Strings rather than the Colour type so the CLI can report the panel without
// importing this package: what a caller wants here is to print it, and a board
// with entirely different lamps should be able to answer the same question.
func (s *SCD) LampSummary() (map[string]string, error) {
	lamps, err := s.Lamps()
	if err != nil {
		return nil, err
	}
	out := make(map[string]string, len(lamps)+1)
	for n, c := range lamps {
		out[n] = c.String()
	}
	if on, berr := s.BeaconOn(); berr == nil {
		out["beacon"] = map[bool]string{true: "on (blue)", false: "off"}[on]
		// The beacon overrides the status lamp's colour in hardware, so a
		// status of green while the beacon is lit means a blue panel light.
		// Say so rather than reporting a colour nobody can see.
		if on {
			if st, ok := out["status"]; ok {
				out["status"] = st + " (showing blue: beacon lit)"
			}
		}
	}
	return out, nil
}

// HealthLamps renders measured health onto the chassis lamps.
//
// Called from the thermal loop, because that is the thing already reading the
// fans and the sensors every cycle. Rendering health from state that was
// measured for another purpose keeps the lamps honest: there is no separate
// notion of health here that could drift from what the cooling is acting on.
//
// The policy:
//
//	fan      green  every bay filled and turning
//	         amber  a bay empty
//	         red    a fitted fan reads zero rpm, or the CPLD stopped answering
//	psu1/2   green  fitted        off  bay empty
//	status   green  fans fine and the hottest sensor below maxC
//	         amber  degraded -- a bay empty, or no sensor readable
//	         red    a fitted fan stopped, or over temperature
//
// An unreadable sensor is amber rather than green. That is the opposite of the
// rule the port LEDs use, where a failed query falls back to lit: a lamp that
// goes amber over a transient is a nuisance, but this box's thermal failure
// mode is silent, and a green status light on a switch whose temperature nobody
// can read is the one lie worth avoiding.
func (s *SCD) HealthLamps(fans []platformhal.Fan, hottestC, maxC int, fanErr error) error {
	m, err := s.loadLamps()
	if err != nil {
		return err
	}

	status, fan := LampGreen, LampGreen
	switch {
	case fanErr != nil || len(fans) == 0:
		fan, status = LampRed, LampRed
	default:
		for _, f := range fans {
			if !f.Present {
				if fan == LampGreen {
					fan = LampAmber
				}
				if status == LampGreen {
					status = LampAmber
				}
				continue
			}
			if f.RPM == 0 {
				fan, status = LampRed, LampRed
			}
		}
	}
	switch {
	case hottestC < 0 && status == LampGreen:
		status = LampAmber // nothing readable: not green, not a fault either
	case hottestC >= maxC:
		status = LampRed
	}

	want := map[string]Colour{"fan": fan, "status": status}
	if present, err := s.PSUPresent(); err == nil {
		// PSUPresent's own names carry question marks because which supply is
		// which is not settled; the lamps are indexed the same way, so a wrong
		// label here is wrong in both places rather than in one.
		want["psu1"] = lampFor(present["psu1?"])
		want["psu2"] = lampFor(present["psu2?"])
	}

	var firstErr error
	for _, l := range m.order {
		c, ok := want[l.Name]
		if !ok {
			continue
		}
		if err := s.SetLamp(l, c); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

func lampFor(present bool) Colour {
	if present {
		return LampGreen
	}
	return LampOff
}

// LampsUnmanaged marks the panel as no longer rendered, when the loop that
// renders it is stopping.
//
// Amber, not red: nothing has failed, and nothing is being watched either. Red
// would send somebody to a rack to look for a fault that is not there, which is
// how a status light stops being believed. The other lamps are left as they
// are -- presence is still presence, and the fans keep turning at full whether
// or not anything is left to describe them.
func (s *SCD) LampsUnmanaged() error {
	m, err := s.loadLamps()
	if err != nil {
		return err
	}
	for _, l := range m.order {
		if l.Name == "status" {
			return s.SetLamp(l, LampAmber)
		}
	}
	return nil
}
