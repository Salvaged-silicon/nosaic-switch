package as4610

import (
	"context"
	"fmt"
	"sort"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
)

// The board controller's registers.
//
// ⚠ EVERY ONE OF THESE IS TRANSCRIBED, NOT DERIVED. They come from EdgeNOS's
// driver for this board, which took them from Open Network Linux's
// accton_as4610_{fan,psu}.c. There is no public document for this CPLD and
// nothing here has been checked against one. Where a reading has already
// disagreed with the map, the disagreement is recorded beside it rather than
// smoothed over.
const (
	regFanPWM    = 0x2b // duty for all fans, low nibble
	regFan1Tach  = 0x2d
	regFan2Tach  = 0x2c
	regPSUStatus = 0x11

	// PHY reset lines. Deasserting all five is what turns the front panel on:
	// the board powers up holding the copper and 10G PHYs in reset, and until
	// they are released every port reports down with no error anywhere.
	regPHYReset1 = 0x07 // copper, ports 1-24
	regPHYReset2 = 0x08 // copper, ports 25-48
	regPHYReset3 = 0x0d // 10G, the SFP+ cages
	regPHYReset4 = 0x19
	regPHYReset5 = 0x1b
)

// phyRelease is the sequence and the values, in EdgeNOS's order.
//
// The order has not been shown to matter and is preserved anyway: it is the
// sequence known to work, and keeping it costs nothing.
var phyRelease = []struct {
	reg  int
	val  byte
	what string
}{
	{regPHYReset1, 0x02, "copper PHY reset (ports 1-24)"},
	{regPHYReset2, 0x02, "copper PHY reset (ports 25-48)"},
	{regPHYReset3, 0x01, "10G PHY reset (SFP+ cages)"},
	{regPHYReset4, 0x00, "PHY reset group 3"},
	{regPHYReset5, 0x00, "PHY reset group 4"},
}

// fanCount and psuCount are fixed for this chassis.
const (
	fanCount = 2
	psuCount = 2

	// The lowest duty this board will command.
	//
	// A fan controller that can be told to stop is one that will eventually be
	// told to stop by a bug, so this is enforced at the write rather than by
	// the caller.
	//
	// ⚠ 50% IS DELIBERATELY HIGH, AND IS A PLACEHOLDER FOR A MEASUREMENT.
	//
	// Nobody has watched this chassis's temperature against its fan duty. What
	// is known is one reading: 49 °C at the board sensor with the duty
	// register at full, which is where this switch powers up and stays,
	// because nothing has ever commanded it otherwise. That says nothing about
	// where it settles at half speed.
	//
	// Declaring a HAL driver is what starts the cooling loop, so the first
	// boot with this driver present will take the fans off full. Doing that on
	// an unmeasured box with a floor of two steps out of eight is how a switch
	// gets quietly hot; four steps costs some noise and no risk. Lower it once
	// somebody has a curve — that is an item in the board's todo.md, not a
	// number to reduce because it seems conservative.
	fanFloorPercent = 50
)

// HAL is the AS4610-54T's platform hardware.
type HAL struct {
	cfg platformhal.Config
	m   *platformhal.I2CMap

	ctrl  bus         // the bus the board controller is on
	cages map[int]bus // cage number (1-based) to its bus
	other map[int]bus // any other bus, by number
}

func init() { platformhal.Register("as4610-cpld", Open) }

// Open connects to the board's controller and checks that something is there.
func Open(cfg platformhal.Config) (platformhal.HAL, error) {
	m := cfg.I2C
	if m == nil {
		return nil, fmt.Errorf("%w: this board declares no platform_hal.i2c map, "+
			"so there is nothing to say where its controller is",
			platformhal.ErrUnsupported)
	}

	h := &HAL{cfg: cfg, m: m, cages: map[int]bus{}, other: map[int]bus{}}

	b, err := h.busFor(m.Controller.Bus)
	if err != nil {
		return nil, err
	}
	h.ctrl = b

	// The board states its bus numbers; this is where that statement is
	// checked. Linux adapter numbers come from probe order, and a kernel
	// change that renumbers them would otherwise turn into a fan tachometer
	// read off a transceiver rather than into an error.
	if m.Adapter != "" {
		if got := adapterName(m.Controller.Bus); got != "" && !strings.Contains(got, m.Adapter) {
			return nil, fmt.Errorf(
				"i2c-%d is %q, and this board says its controller is on %q. "+
					"The buses have renumbered; fix platform_hal.i2c in board.yml "+
					"rather than letting this read the wrong device",
				m.Controller.Bus, got, m.Adapter)
		}
	}

	// One read, so that "no controller" is an error here rather than a
	// wrong number later. A CPLD that is not there does not fail loudly:
	// every access is refused, which reads as a broken bus.
	if _, err := h.ctrl.ReadReg(m.Controller.Addr, regPSUStatus); err != nil {
		return nil, fmt.Errorf("no board controller at %#02x on i2c-%d: %w",
			m.Controller.Addr, m.Controller.Bus, err)
	}
	return h, nil
}

func (h *HAL) busFor(n int) (bus, error) {
	if b, ok := h.other[n]; ok {
		return b, nil
	}
	b, err := openBus(n)
	if err != nil {
		return nil, err
	}
	h.other[n] = b
	return b, nil
}

// Board identifies the hardware from its own EEPROM.
func (h *HAL) Board() (platformhal.Identity, error) {
	if h.m.EEPROM == nil {
		return platformhal.Identity{}, fmt.Errorf(
			"%w: this board declares no identity EEPROM", platformhal.ErrUnsupported)
	}
	b, err := h.busFor(h.m.EEPROM.Bus)
	if err != nil {
		return platformhal.Identity{}, err
	}
	// 256 bytes: a 24c04 is 512 and ONIE's own tooling reads the first page.
	// Reading the whole part would double the transaction for a header that
	// declares its own length anyway.
	raw, err := b.ReadAt(h.m.EEPROM.Addr, 0, 256)
	if err != nil {
		return platformhal.Identity{}, fmt.Errorf("reading the board EEPROM: %w", err)
	}
	return decodeONIE(raw)
}

// ResetState reports whether a line is asserted.
//
// The switch chip's resets are the ones this interface was built for, and this
// board has neither: the ASIC is the same die as the CPU, so it is out of
// reset before any software runs and there is no line to hold it. Saying so is
// more useful than inventing an answer.
func (h *HAL) ResetState(r platformhal.Reset) (bool, error) {
	return false, fmt.Errorf("%w: the switch chip is on the CPU's own die and "+
		"has no board reset line (%s)", platformhal.ErrUnsupported, r)
}

// ReleaseSwitchChip brings the front panel up.
//
// On the Arista boards this releases the ASIC and waits for it to appear on
// PCI. Here the chip was never held, and the thing that *is* held is the
// external PHYs in front of the ports — so this does that instead. The name
// stays because the contract is "make the forwarding hardware reachable", and
// on this board that is the panel.
func (h *HAL) ReleaseSwitchChip(ctx context.Context) error {
	var failed []string

	for _, r := range phyRelease {
		if err := ctx.Err(); err != nil {
			return err
		}
		if err := h.ctrl.WriteReg(h.m.Controller.Addr, r.reg, r.val); err != nil {
			failed = append(failed, fmt.Sprintf("%#02x (%s)", r.reg, r.what))
		}
	}
	if len(failed) > 0 {
		return fmt.Errorf("the front panel is only partly released; these writes "+
			"were refused: %s. Expect ports down", strings.Join(failed, ", "))
	}
	return nil
}

// Watchdog: the SoC has one, and it is not reachable from here.
//
// There is an sp805 at 0x18039000, and the board's device tree disables it
// because mainline has no driver for this SoC's APB clock and the node would
// not probe. When that changes it becomes an ordinary /dev/watchdog and this
// method becomes a thin wrapper over it, rather than anything on the CPLD.
func (h *HAL) Watchdog() (platformhal.Watchdog, error) {
	return nil, fmt.Errorf("%w: the SoC's watchdog is disabled in this board's "+
		"device tree, because mainline has no driver for its clock",
		platformhal.ErrUnsupported)
}

// Temperatures reports every sensor the board declares, in millidegrees.
func (h *HAL) Temperatures() (map[string]int, error) {
	if len(h.m.Sensors) == 0 {
		return nil, fmt.Errorf("%w: this board declares no temperature sensors",
			platformhal.ErrUnsupported)
	}
	out := map[string]int{}
	var errs []string

	for _, s := range h.m.Sensors {
		b, err := h.busFor(s.Bus)
		if err != nil {
			errs = append(errs, fmt.Sprintf("%s: %v", s.Name, err))
			continue
		}
		w, err := b.ReadWord(s.Addr, platformhal.I2CParts[s.Part])
		if err != nil {
			errs = append(errs, fmt.Sprintf("%s: %v", s.Name, err))
			continue
		}
		out[s.Name] = lm77MilliC(w)
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no sensor could be read: %s", strings.Join(errs, "; "))
	}
	return out, nil
}

// lm77MilliC decodes an LM77 temperature register.
//
// The reading is the top 13 bits of a big-endian word in half degrees, with
// the bottom three bits status. Two details, both of which have a wrong answer
// that looks right:
//
// It is SIGNED. Shifting a value already widened to int reads -1 °C as
// 4095.5 °C, and a switch reporting four thousand degrees is believed by a
// thermal loop long before it is believed by a person.
//
// It DIVIDES rather than shifts, which is the kernel's own arithmetic:
// drivers/hwmon/lm77.c has LM77_TEMP_FROM_REG as (reg / 8) * 500. Division
// truncates toward zero and an arithmetic shift toward negative infinity, so
// the two disagree by half a degree just below freezing — where this board
// will never be, but where an operator comparing this against
// /sys/class/hwmon/hwmon0/temp1_input would find two numbers and no
// explanation. Matching the reference implementation costs nothing.
func lm77MilliC(w uint16) int {
	return int(int16(w)/8) * 500
}

// PSUPresent reports which supplies are fitted.
func (h *HAL) PSUPresent() (map[string]bool, error) {
	st, err := h.ctrl.ReadReg(h.m.Controller.Addr, regPSUStatus)
	if err != nil {
		return nil, fmt.Errorf("reading power supply status: %w", err)
	}
	out := map[string]bool{}
	for i := 0; i < psuCount; i++ {
		out[fmt.Sprintf("psu%d", i+1)] = st&(1<<(i*2)) != 0
	}
	return out, nil
}

// PSUStatus is presence and power, which are different questions: a supply
// that is fitted with nothing plugged into it is the normal state of a second
// PSU, not a fault.
type PSUStatus struct {
	Index   int
	Present bool
	OK      bool
	Raw     byte
}

// PSUs reports both bits for every supply.
//
// ⚠ The decode is the vendor's and has not been checked against a switch with
// one supply deliberately unplugged. The AS5610's equivalent turned out to be
// two registers rather than two fields of one, with presence active low, and
// the vendor's own kernel driver had it one way while its Python layer had it
// the other. Read this as the best available guess until somebody pulls a
// power lead and watches which bit moves.
func (h *HAL) PSUs() ([]PSUStatus, error) {
	st, err := h.ctrl.ReadReg(h.m.Controller.Addr, regPSUStatus)
	if err != nil {
		return nil, fmt.Errorf("reading power supply status: %w", err)
	}
	out := make([]PSUStatus, 0, psuCount)
	for i := 0; i < psuCount; i++ {
		out = append(out, PSUStatus{
			Index:   i + 1,
			Present: st&(1<<(i*2)) != 0,
			OK:      st&(1<<(i*2+1)) != 0,
			Raw:     st,
		})
	}
	return out, nil
}

// ---------------------------------------------------------------- cooling

// FanCount is how many bays the chassis has.
func (h *HAL) FanCount() int { return fanCount }

// FanFloorPercent is the lowest duty this board will command.
func (h *HAL) FanFloorPercent() int { return fanFloorPercent }

// fanRegToDuty turns the duty register into a percentage.
//
// EdgeNOS's formula, which implies the register runs 0..8 rather than 0..15:
// eight steps of 12.5%, with 8 meaning full speed. Values above 8 would decode
// past 100%, so they are clamped rather than reported — a duty of 188% is not
// a reading, it is a sign the map is wrong, and it is reported through Raw.
func fanRegToDuty(reg byte) int {
	pct := (int(reg&0x0f)*125 + 5) / 10
	if pct > 100 {
		return 100
	}
	return pct
}

// fanDutyToReg is the inverse, rounded to the nearest step.
func fanDutyToReg(pct int) byte {
	if pct < 0 {
		pct = 0
	}
	if pct > 100 {
		pct = 100
	}
	// Eight steps of 12.5%, rounded rather than truncated: truncating makes
	// every requested duty come out one step low, which a thermal loop
	// compensates for by asking for more, indefinitely.
	return byte((pct*8 + 50) / 100)
}

// Fans reports the trays.
//
// ⚠ ON THE ONE UNIT THIS HAS BEEN READ ON, THE TACHOMETERS READ ZERO WHILE THE
// DUTY REGISTER SAYS FULL SPEED, and the fans are certainly turning. Either the
// decode is wrong, or those registers need something done first, or they are
// not the registers.
//
// So RPM is reported as read and Raw carries the register beside it. It is
// deliberately not smoothed into something plausible: a cooling loop built on
// a tachometer that reads zero is a loop that will decide the fans have failed,
// and internal/thermal takes its input from the temperature rather than from
// this for exactly that reason.
func (h *HAL) Fans() ([]platformhal.Fan, error) {
	addr := h.m.Controller.Addr
	pwm, err := h.ctrl.ReadReg(addr, regFanPWM)
	if err != nil {
		return nil, fmt.Errorf("reading the fan duty: %w", err)
	}
	duty := fanRegToDuty(pwm)

	out := make([]platformhal.Fan, 0, fanCount)
	for i, reg := range []int{regFan1Tach, regFan2Tach} {
		f := platformhal.Fan{Index: i + 1, Present: true, Percent: duty}
		tach, err := h.ctrl.ReadReg(addr, reg)
		if err != nil {
			f.Raw = fmt.Sprintf("pwm=%#02x tach=unreadable", pwm)
			out = append(out, f)
			continue
		}
		f.RPM = tachToRPM(tach)
		f.Raw = fmt.Sprintf("pwm=%#02x tach=%#02x", pwm, tach)
		out = append(out, f)
	}
	return out, nil
}

// tachToRPM is Open Network Linux's conversion for this board.
func tachToRPM(raw byte) int { return int(raw) * 379 * 60 / 2 / 100 }

// SetFanPercent commands every fan, clamped to the board's floor.
func (h *HAL) SetFanPercent(percent int) (int, error) {
	if percent < fanFloorPercent {
		percent = fanFloorPercent
	}
	if err := h.ctrl.WriteReg(h.m.Controller.Addr, regFanPWM, fanDutyToReg(percent)); err != nil {
		// One register drives every fan here, so a refused write is all of
		// them rather than one bay. Saying "2 refused" would suggest the
		// board has per-bay control that it does not.
		return fanCount, fmt.Errorf("commanding the fans: %w", err)
	}
	return 0, nil
}

// Close releases every bus this HAL opened.
func (h *HAL) Close() error {
	var first error
	seen := map[bus]bool{}
	nums := make([]int, 0, len(h.other))
	for n := range h.other {
		nums = append(nums, n)
	}
	sort.Ints(nums)
	for _, n := range nums {
		b := h.other[n]
		if seen[b] {
			continue
		}
		seen[b] = true
		if err := b.Close(); err != nil && first == nil {
			first = err
		}
	}
	return first
}
