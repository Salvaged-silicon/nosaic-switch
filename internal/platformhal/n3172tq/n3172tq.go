/* SPDX-License-Identifier: Apache-2.0 */

// Package hwmon is the platform HAL for a board whose auxiliary parts the
// kernel drives.
//
// # WHY THIS EXISTS BESIDE scd
//
// The two drivers answer the same questions about different hardware, and the
// difference is which side owns the bus. Arista's SCD is an FPGA whose SMBus
// this tree drives from userspace: scd does the transfers and decodes the
// parts itself. On a board like the Nexus 3172TQ there is nothing to drive --
// the sensors and supplies are ordinary i2c devices with in-tree drivers, and
// by the time anything asks, the kernel has already read them. Re-implementing
// an ADT7462 decode in Go when drivers/hwmon/adt7462.c is already bound to the
// part would be work done twice and wrong the second time.
//
// So this driver does no I/O of its own: it reads /sys/class/hwmon. What the
// board still has to state is which chip and channel is which location, which
// is Map, because "temp1" is not a place on a chassis.
//
// # WHAT IT DOES NOT DO
//
// ReleaseSwitchChip and the watchdog are ErrUnsupported here. On this board
// the switch chip is already on the PCI bus -- nothing holds it in reset that
// we can reach -- and a HAL that pretends to arm a watchdog it cannot arm is
// worse than one that says it cannot: a caller who believes it is protected
// and is not is the case the Watchdog comment warns about.
package n3172tq

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"context"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// sysfsRoot is a variable so the tests can point it at a tree on disk. A
// driver that can only be exercised on the switch is one that gets exercised
// on the switch, which is the most expensive place to find a bug.
var sysfsRoot = "/sys"

type hal struct {
	d *Data
	// root is where /sys is, captured at Open so a test tree stays put.
	root string
}

// Open returns the HAL. It does not require the parts to be present: a board
// that has not instantiated its i2c devices yet should report no sensors, not
// refuse to open, because `nosaic platform status` is exactly what someone
// runs to find out why there are none.
func Open(cfg platformhal.Config) (platformhal.HAL, error) {
	d, ok := cfg.BoardData.(*Data)
	if !ok || d == nil {
		return nil, fmt.Errorf("%w: this board's platform data is %T, not "+
			"*n3172tq.Data -- check platform_hal.n3172tq in board.yml",
			platformhal.ErrUnsupported, cfg.BoardData)
	}
	if err := d.Validate(); err != nil {
		return nil, fmt.Errorf("this board's platform data: %w", err)
	}
	if d.Hwmon == nil {
		return nil, fmt.Errorf("%w: this board names no hwmon parts, so nothing "+
			"says which chip is which sensor", platformhal.ErrUnsupported)
	}
	return &hal{d: d, root: sysfsRoot}, nil
}

// chipDirs returns the hwmon directories whose name matches, in a stable
// order. More than one is normal: two identical supplies are two pmbus chips.
func (h *hal) chipDirs(name string) ([]string, error) {
	glob := filepath.Join(h.root, "class", "hwmon", "hwmon*")
	all, err := filepath.Glob(glob)
	if err != nil {
		return nil, err
	}
	var out []string
	for _, d := range all {
		n, err := os.ReadFile(filepath.Join(d, "name"))
		if err != nil {
			continue
		}
		if strings.TrimSpace(string(n)) == name {
			out = append(out, d)
		}
	}
	return out, nil
}

func readInt(path string) (int, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	return strconv.Atoi(strings.TrimSpace(string(b)))
}

// Temperatures reports each named sensor in millidegrees, which is what hwmon
// already uses -- no conversion, so no rounding to argue about.
func (h *hal) Temperatures() (map[string]int, error) {
	out := map[string]int{}
	for _, s := range h.d.Hwmon.Sensors {
		dirs, err := h.chipDirs(s.Chip)
		if err != nil || len(dirs) == 0 {
			continue
		}
		p := filepath.Join(dirs[0], fmt.Sprintf("temp%d_input", s.Channel))
		v, err := readInt(p)
		if err != nil {
			// A channel the chip does not populate is left out rather than
			// reported as 0: a thermal loop reading 0 °C for the hottest part
			// of the board would wind the fans down.
			continue
		}
		out[s.Name] = v
	}
	// The switch die, which is not an i2c part and is hotter than every one
	// of the sensors above. Absent whenever the datapath is not running, so
	// it is added if it can be read and passed over silently if not -- a
	// board with no datapath yet is still a board that must cool itself.
	if milli, ok := asicTempMilliC(); ok {
		out["ASIC"] = milli
	}

	if len(out) == 0 {
		return nil, fmt.Errorf("no sensors readable: are this board's i2c "+
			"devices instantiated? see /etc/nosaic/i2c-devices.sh (%w)",
			platformhal.ErrUnsupported)
	}
	return out, nil
}

// psuDir finds the hwmon directory for one supply.
//
// Both supplies are the same driver at the same address, so the chip name and
// the address together are not enough -- what tells them apart is the mux
// channel. That is resolved through the mux's own channel-N symlink, the same
// interface the boot-time instantiation uses, rather than by assuming the
// child adapters are numbered parent+1+N.
func (h *hal) psuDir(p PSU) (string, error) {
	dirs, err := h.chipDirs(p.Chip)
	if err != nil {
		return "", err
	}
	want := fmt.Sprintf("-%04x", p.Address)
	var busPrefix string
	if p.Channel != nil {
		bus, err := h.muxChannelBus(*p.Channel)
		if err != nil {
			return "", err
		}
		busPrefix = fmt.Sprintf("%d-", bus)
	}
	for _, d := range dirs {
		// hwmonN/device points at the i2c device, named <bus>-<4 hex addr>.
		dev, err := os.Readlink(filepath.Join(d, "device"))
		if err != nil {
			continue
		}
		base := filepath.Base(dev)
		if !strings.HasSuffix(base, want) {
			continue
		}
		if busPrefix != "" && !strings.HasPrefix(base, busPrefix) {
			continue
		}
		return d, nil
	}
	return "", fmt.Errorf("%w: no %s at address %#x on channel %v",
		platformhal.ErrUnsupported, p.Chip, p.Address, p.Channel)
}

// muxChannelBus resolves a mux channel to its i2c adapter number.
func (h *hal) muxChannelBus(ch int) (int, error) {
	if h.d.I2C == nil || h.d.I2C.Mux == nil {
		return 0, fmt.Errorf("%w: this board names no i2c mux, so channel %d "+
			"cannot be resolved", platformhal.ErrUnsupported, ch)
	}
	// The mux device directory is <parent bus>-00<addr>; the parent is
	// whichever adapter it was instantiated on, so it is found rather than
	// assumed.
	pattern := filepath.Join(h.root, "bus", "i2c", "devices",
		fmt.Sprintf("*-%04x", h.d.I2C.Mux.Address))
	muxes, err := filepath.Glob(pattern)
	if err != nil || len(muxes) == 0 {
		return 0, fmt.Errorf("%w: the mux at %#x is not instantiated",
			platformhal.ErrUnsupported, h.d.I2C.Mux.Address)
	}
	link := filepath.Join(muxes[0], fmt.Sprintf("channel-%d", ch))
	target, err := os.Readlink(link)
	if err != nil {
		return 0, fmt.Errorf("%w: mux channel %d has no adapter",
			platformhal.ErrUnsupported, ch)
	}
	base := strings.TrimPrefix(filepath.Base(target), "i2c-")
	return strconv.Atoi(base)
}

// PSUPresent reports which supplies answer. A supply that is fitted but has no
// AC still answers its PMBus registers -- measured on this chassis, where
// NX-OS reports PS1 as "fail/not-powered-up" and our own in1_input on that
// channel reads 0 while the other reads 120500. Both chips bind either way, so
// this is presence, in the sense the interface asks for ("which power supplies
// are fitted"), and says nothing about health.
func (h *hal) PSUPresent() (map[string]bool, error) {
	if len(h.d.Hwmon.PSUs) == 0 {
		return nil, fmt.Errorf("%w: this board names no supplies", platformhal.ErrUnsupported)
	}
	out := map[string]bool{}
	for _, p := range h.d.Hwmon.PSUs {
		_, err := h.psuDir(p)
		out[p.Name] = err == nil
	}
	return out, nil
}

// Board is what the board says it is. The ID PROM is an at24 on this board and
// its layout is the vendor's, not decoded yet, so this reports what is known
// rather than inventing a serial.
// Board reads the identity out of the board's own ID PROM.
//
// The chassis record is the one reported: its serial is what is printed on
// the asset label and quoted in a support case, while the board record's
// serial names the PCB inside. Both are decoded; only one can be returned
// through this interface, and the label is the one somebody reading
// `nosaic platform status` is holding.
func (h *hal) Board() (platformhal.Identity, error) {
	path, err := h.eepromPath()
	if err != nil {
		return platformhal.Identity{}, fmt.Errorf("%w: %s",
			platformhal.ErrUnsupported, err)
	}
	image, err := os.ReadFile(path)
	if err != nil {
		return platformhal.Identity{}, fmt.Errorf("%w: %s is not readable "+
			"(it is root-only): %v", platformhal.ErrUnsupported, path, err)
	}
	d, err := parseIDPROMRecord(image, idpromChassisRecord)
	if err != nil {
		return platformhal.Identity{}, fmt.Errorf("%w: %s",
			platformhal.ErrUnsupported, err)
	}
	if d.Model == "" && d.Serial == "" {
		return platformhal.Identity{}, fmt.Errorf("%w: %s has the right magic "+
			"but no readable model or serial, so the field offsets are wrong "+
			"for this unit", platformhal.ErrUnsupported, path)
	}
	return platformhal.Identity{
		Model:    d.Model,
		Serial:   d.Serial,
		Revision: d.Rev,
		SID:      d.Part,
	}, nil
}

func (h *hal) ResetState(platformhal.Reset) (bool, error) {
	return false, fmt.Errorf("%w: no reset lines are reachable on this board",
		platformhal.ErrUnsupported)
}

func (h *hal) ReleaseSwitchChip(context.Context) error {
	return fmt.Errorf("%w: the switch chip is already on the PCI bus on this "+
		"board; nothing we can reach holds it in reset", platformhal.ErrUnsupported)
}

func (h *hal) Watchdog() (platformhal.Watchdog, error) {
	return nil, fmt.Errorf("%w: no watchdog is reachable on this board",
		platformhal.ErrUnsupported)
}
