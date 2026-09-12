package scd

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// The TI DS100KR800 repeater that sits between the switch ASIC's SerDes and
// some of the front-panel cages.
//
// ⚠ AN UNPROGRAMMED REPEATER PASSES NOTHING, AND NOTHING SAYS SO.
//
// Eight channels: two ports' worth of four lanes, in ONE direction -- from the
// host to the module. So a cage behind it receives perfectly while its
// transmit never arrives, which presents as a link this end reports up and the
// far end reports down, with no error on either. Releasing its reset is not
// enough; out of reset and unconfigured it still conditions nothing.
//
// # WHERE THE NUMBERS COME FROM, AND WHICH ARE HERE
//
// The register offsets below are from TI's public datasheet (SNLS340E, Table
// 6). They are the same for every board carrying the part, so they belong in
// the driver.
//
// The VALUES are not. Output amplitude and de-emphasis are per-trace-length
// tuning that the board vendor established for one PCB, they live in that
// board's own description file, and they are not ours to publish -- so they
// are read at runtime from a file generated on the switch, exactly like the
// port map and the SerDes polarity. With no file this refuses to program
// rather than guessing: an unprogrammed repeater is a dead port, obvious and
// annoying, and a wrongly programmed one is a link that works until it does
// not.
const (
	// retimerPowerDown is channel power-down, one bit per channel. 0x00 is
	// every channel on.
	retimerPowerDown = 0x01
	// retimerRegCtl must enable register control before any per-channel write
	// lands: in pin mode the writes are accepted and ignored.
	retimerRegCtl = 0x06
	// retimerPinCtl takes the strapping pins out of the picture.
	retimerPinCtl = 0x08
)

// retimerChannelBase is the EQ register for each of the eight channels;
// amplitude is base+1 and de-emphasis base+2.
//
// ⚠ NOT A UNIFORM STRIDE. Channels 0-3 step by 7 and then there is an eight-
// byte gap at the bank boundary. Computing these with a stride reads three of
// the eight channels at the wrong address.
var retimerChannelBase = [8]int{0x0f, 0x16, 0x1d, 0x24, 0x2c, 0x33, 0x3a, 0x41}

// RetimerTuning is the board's own values for the part.
type RetimerTuning struct {
	PinControl, DisableCRC byte
	VODLow, VODHigh        byte
	DEMLow, DEMHigh        byte
	EQ                     byte
}

// RetimerConfPath is where the generated tuning lives on a running switch.
const RetimerConfPath = "/etc/nosaic/retimer.conf"

// LoadRetimerTuning reads the board's tuning, or says why it cannot.
func LoadRetimerTuning(path string) (*RetimerTuning, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("%w: no %s. The repeater's tuning is this "+
			"board's own, from its description file, and is not shipped -- "+
			"generate it on this switch with tools/mkretimer.sh", err, path)
	}
	defer f.Close()

	got := map[string]byte{}
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			if k, v, ok = strings.Cut(line, " "); !ok {
				continue
			}
		}
		n, err := strconv.ParseUint(strings.TrimSpace(strings.TrimPrefix(
			strings.TrimSpace(v), "0x")), 16, 8)
		if err != nil {
			// Also accept plain decimal, which is how the board's own
			// description states them.
			d, derr := strconv.ParseUint(strings.TrimSpace(v), 10, 8)
			if derr != nil {
				return nil, fmt.Errorf("%s: %q is not a byte", path, v)
			}
			n = d
		}
		got[strings.TrimSpace(k)] = byte(n)
	}
	need := []string{"pin_control", "disable_crc", "vod_low", "vod_high", "dem_low", "dem_high", "eq"}
	for _, k := range need {
		if _, ok := got[k]; !ok {
			return nil, fmt.Errorf("%s: no %s", path, k)
		}
	}
	return &RetimerTuning{
		PinControl: got["pin_control"], DisableCRC: got["disable_crc"],
		VODLow: got["vod_low"], VODHigh: got["vod_high"],
		DEMLow: got["dem_low"], DEMHigh: got["dem_high"], EQ: got["eq"],
	}, nil
}

func (s *SCD) retimer() (*platformhal.SMBusAddr, error) {
	if s.smbusMap == nil || s.smbusMap.Retimer == nil {
		return nil, fmt.Errorf("%w: this board states no repeater "+
			"(platform_hal.smbus.retimer)", platformhal.ErrUnsupported)
	}
	return s.smbusMap.Retimer, nil
}

// ProgramRetimer writes the board's tuning to the repeater.
//
// Order matters and is not arbitrary: register control is enabled and the pins
// are overridden BEFORE any per-channel value, or every per-channel write is
// accepted and discarded.
func (s *SCD) ProgramRetimer(t *RetimerTuning, log func(string, ...any)) error {
	a, err := s.retimer()
	if err != nil {
		return err
	}
	m := s.smb()
	w := func(reg int, v byte, what string) error {
		if err := m.WriteReg(a.Accel, a.Bus, a.Addr, reg, v); err != nil {
			return fmt.Errorf("repeater %#02x reg %#02x (%s): %w", a.Addr, reg, what, err)
		}
		return nil
	}

	if err := w(retimerRegCtl, t.DisableCRC, "enable register control"); err != nil {
		return err
	}
	if err := w(retimerPinCtl, t.PinControl, "override the strapping pins"); err != nil {
		return err
	}
	if err := w(retimerPowerDown, 0x00, "power on every channel"); err != nil {
		return err
	}
	for i, base := range retimerChannelBase {
		vod, dem := t.VODLow, t.DEMLow
		if i >= 4 {
			vod, dem = t.VODHigh, t.DEMHigh
		}
		if err := w(base, t.EQ, "equalisation"); err != nil {
			return err
		}
		if err := w(base+1, vod, "output amplitude"); err != nil {
			return err
		}
		if err := w(base+2, dem, "de-emphasis"); err != nil {
			return err
		}
	}
	if log != nil {
		log("repeater at %#02x (a%d b%d): 8 channels programmed",
			a.Addr, a.Accel, a.Bus)
	}
	return nil
}

// RetimerReport reads back what the repeater currently holds.
//
// ⚠ Judge a write by effect, not only by read-back: de-emphasis reads with its
// top bit set by the hardware, so a byte written as 0x01 reads as 0x81.
func (s *SCD) RetimerReport() (string, error) {
	a, err := s.retimer()
	if err != nil {
		return "", err
	}
	m := s.smb()
	rd := func(reg int) string {
		v, err := m.ReadReg(a.Accel, a.Bus, a.Addr, reg)
		if err != nil {
			return "??"
		}
		return fmt.Sprintf("%#02x", v)
	}
	var b strings.Builder
	fmt.Fprintf(&b, "repeater %#02x on a%d b%d\n", a.Addr, a.Accel, a.Bus)
	fmt.Fprintf(&b, "  power-down %s  register-control %s  pin-control %s\n",
		rd(retimerPowerDown), rd(retimerRegCtl), rd(retimerPinCtl))
	for i, base := range retimerChannelBase {
		fmt.Fprintf(&b, "  ch%d  eq %s  amplitude %s  de-emphasis %s\n",
			i, rd(base), rd(base+1), rd(base+2))
	}
	return b.String(), nil
}
