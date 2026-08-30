package main

import (
	"context"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"text/tabwriter"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal/scd"
)

const platformUsage = `usage: nosaic platform <command>

  status               what the board reports about itself
  release-asic         take the switch chip out of reset and wait for it
  asic                 what the switch chip says about itself (read-only)
  transceivers         which front-panel cages have modules in them
  tx <cage> on|off     turn a cage's transmitter on or off
  schan selftest       prove S-Channel reaches the chip (read-only)
  schan read <addr>    one register read over S-Channel
  watchdog status      whether the hardware watchdog is armed
  watchdog arm <ms>    arm it; the action is a power cycle
  watchdog disarm      stop it -- only with a console attached
  watchdog raw <hex>   write the register verbatim (bring-up only)

Reads the running board's id from /etc/nosaic/board, or --board <id>.
`

func platformCmd(args []string) error {
	boardID := ""
	var rest []string
	for i := 0; i < len(args); i++ {
		if args[i] == "--board" && i+1 < len(args) {
			boardID = args[i+1]
			i++
			continue
		}
		rest = append(rest, args[i])
	}
	if len(rest) == 0 {
		fmt.Print(platformUsage)
		return nil
	}

	hal, b, err := openBoardHAL(boardID)
	if err != nil {
		return err
	}
	if c, ok := hal.(interface{ Close() error }); ok {
		defer c.Close()
	}

	switch rest[0] {
	case "status":
		return platformStatus(hal, b)
	case "release-asic":
		return releaseASIC(hal)
	case "asic":
		return probeASIC(hal)
	case "schan":
		return schanCmd(b, rest[1:])
	case "transceivers", "xcvr":
		return showTransceivers(hal)
	case "tx":
		return setCageTX(hal, rest[1:])
	case "watchdog":
		return watchdogCmd(hal, rest[1:])
	}
	return fmt.Errorf("unknown platform command %q", rest[0])
}

// openBoardHAL finds which board this is and opens its driver.
//
// The board is identified from the running system rather than assumed, because
// the wrong board's driver means writing the wrong registers on real hardware.
func openBoardHAL(id string) (platformhal.HAL, *board.Board, error) {
	// A NOSaic image carries its own board description, so on the switch this
	// needs no argument and cannot be told the wrong board. Off the switch --
	// in the source tree -- the board must be named, because there is nothing
	// to identify and guessing would mean writing one board's registers on
	// another's hardware.
	if id == "" {
		b, err := board.Load(installedBoardFile)
		if err != nil {
			return nil, nil, fmt.Errorf("cannot tell which board this is: %w "+
				"(on a NOSaic image this is written at build time; "+
				"elsewhere pass --board <id>)", err)
		}
		return openFor(b)
	}
	boards, err := board.LoadAll(repoRoot())
	if err != nil {
		return nil, nil, err
	}
	for _, b := range boards {
		if b.ID == id {
			return openFor(b)
		}
	}
	return nil, nil, fmt.Errorf("no board port named %q", id)
}

// installedBoardFile is where a NOSaic image records what hardware it is on.
var installedBoardFile = "/etc/nosaic/board.yml"

func openFor(b *board.Board) (platformhal.HAL, *board.Board, error) {
	hal, err := platformhal.Open(b.PlatformHAL.Driver, b.PlatformHAL.PCI, b.PlatformHAL.ASICPCI)
	if err != nil {
		return nil, nil, err
	}
	return hal, b, nil
}

func platformStatus(hal platformhal.HAL, b *board.Board) error {
	w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
	fmt.Fprintf(w, "board\t%s\n", b.ID)

	// Every line below reports what the hardware said or why it could not
	// say, never a plausible-looking default. A HAL that invents a reading
	// makes the box unfalsifiable.
	if id, err := hal.Board(); err != nil {
		fmt.Fprintf(w, "identity\t— %v\n", err)
	} else {
		fmt.Fprintf(w, "identity\t%s  serial %s  rev %s  sid %s\n",
			id.Model, id.Serial, id.Revision, id.SID)
	}

	for _, r := range []platformhal.Reset{platformhal.ResetSwitchCore, platformhal.ResetSwitchPCIe} {
		held, err := hal.ResetState(r)
		switch {
		case err != nil:
			fmt.Fprintf(w, "reset %s\t— %v\n", r, err)
		case held:
			fmt.Fprintf(w, "reset %s\tHELD in reset\n", r)
		default:
			fmt.Fprintf(w, "reset %s\treleased\n", r)
		}
	}

	if wd, err := hal.Watchdog(); err != nil {
		fmt.Fprintf(w, "watchdog\t— %v\n", err)
	} else if armed, ms, err := wd.Armed(); err != nil {
		fmt.Fprintf(w, "watchdog\t— %v\n", err)
	} else if armed {
		fmt.Fprintf(w, "watchdog\tarmed, %d ms, power-cycles on expiry\n", ms)
	} else {
		fmt.Fprintf(w, "watchdog\tNOT armed\n")
	}

	temps, err := hal.Temperatures()
	if err != nil && len(temps) == 0 {
		fmt.Fprintf(w, "temperature\t— %v\n", err)
	}
	names := make([]string, 0, len(temps))
	for n := range temps {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		fmt.Fprintf(w, "temp %s\t%.1f °C\n", n, float64(temps[n])/1000)
	}

	if psus, err := hal.PSUPresent(); err != nil {
		fmt.Fprintf(w, "psu\t— %v\n", err)
	} else {
		for _, n := range sortedKeys(psus) {
			state := "absent"
			if psus[n] {
				state = "present"
			}
			fmt.Fprintf(w, "psu %s\t%s\n", n, state)
		}
	}
	return w.Flush()
}

// releaseASIC brings the switch chip onto the bus.
//
// The watchdog is checked first and the answer is only a warning: releasing a
// reset is not itself dangerous, but everything that follows it is, and a
// switch with no automatic recovery is a switch that gets recovered by hand.
func releaseASIC(hal platformhal.HAL) error {
	if wd, err := hal.Watchdog(); err == nil {
		if armed, _, err := wd.Armed(); err == nil && !armed {
			fmt.Fprintln(os.Stderr,
				"warning: the watchdog is not armed. If this wedges the box, "+
					"recovery is manual. Arm it with: nosaic platform watchdog arm 60000")
		}
	}

	// Wire the driver's trace to the console. This is bring-up on hardware
	// that is not in the vendor's own open tree, so what the registers did is
	// the result, not a debugging aid.
	if t, ok := hal.(interface{ SetTrace(func(string, ...any)) }); ok {
		t.SetTrace(func(f string, a ...any) {
			fmt.Printf("  scd: "+f+"\n", a...)
		})
	}

	fmt.Println("releasing the switch chip from reset...")
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := hal.ReleaseSwitchChip(ctx); err != nil {
		return err
	}
	fmt.Println("the switch chip is on the bus.")
	return nil
}

func watchdogCmd(hal platformhal.HAL, args []string) error {
	wd, err := hal.Watchdog()
	if err != nil {
		return err
	}
	if len(args) == 0 {
		args = []string{"status"}
	}
	switch args[0] {
	case "status":
		armed, ms, err := wd.Armed()
		if err != nil {
			return err
		}
		// The raw value first, because the decode rests on a timeout model
		// established by measurement rather than from the vendor's driver.
		if r, ok := wd.(interface{ Raw() uint32 }); ok {
			fmt.Printf("register  %#08x\n", r.Raw())
		}
		if armed {
			fmt.Printf("armed, %d ms, power-cycles on expiry\n", ms)
		} else {
			fmt.Println("not armed")
		}
		return nil

	case "arm":
		if len(args) < 2 {
			return fmt.Errorf("watchdog arm needs a timeout in milliseconds")
		}
		ms, err := strconv.Atoi(args[1])
		if err != nil {
			return fmt.Errorf("timeout %q is not a number of milliseconds", args[1])
		}
		if err := wd.Arm(ms); err != nil {
			return err
		}
		// Read back rather than report success from the write having returned.
		armed, got, err := wd.Armed()
		if err != nil || !armed {
			return fmt.Errorf("armed the watchdog but it does not read back as armed")
		}
		fmt.Printf("armed, %d ms. It must be petted before then or the board power-cycles.\n", got)
		return nil

	case "raw":
		// Deliberately awkward to reach and loudly labelled. Writing this
		// register wrong either removes the recovery net or power-cycles the
		// box under whoever is working on it.
		if len(args) < 2 {
			return fmt.Errorf("watchdog raw needs a 32-bit value in hex, e.g. 0xc0001770")
		}
		v, err := strconv.ParseUint(strings.TrimPrefix(args[1], "0x"), 16, 32)
		if err != nil {
			return fmt.Errorf("%q is not a 32-bit hex value", args[1])
		}
		rw, ok := wd.(interface{ WriteRaw(uint32) uint32 })
		if !ok {
			return fmt.Errorf("%w: this board's watchdog has no raw access", platformhal.ErrUnsupported)
		}
		fmt.Printf("writing %#08x to the watchdog register\n", uint32(v))
		fmt.Printf("readback  %#08x\n", rw.WriteRaw(uint32(v)))
		return nil

	case "disarm":
		if err := wd.Disarm(); err != nil {
			return err
		}
		fmt.Println("disarmed. There is no automatic recovery now.")
		return nil
	}
	return fmt.Errorf("unknown watchdog command %q", args[0])
}

func sortedKeys(m map[string]bool) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// probeASIC reports the switch chip's identity and whether it answers MMIO.
//
// The question it exists to answer is narrow and worth stating: everything
// known about this ASIC was learned after a kexec from the vendor OS, which
// leaves it in a state a standalone boot does not reproduce. "It answered last
// time" is not evidence for the path NOSaic takes.
func probeASIC(hal platformhal.HAL) error {
	p, ok := hal.(interface {
		ProbeASIC() (*scd.ASICProbe, error)
	})
	if !ok {
		return fmt.Errorf("%w: this board has no ASIC probe", platformhal.ErrUnsupported)
	}
	r, err := p.ProbeASIC()
	if err != nil {
		return err
	}

	w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
	fmt.Fprintf(w, "pci\t%s\n", r.PCI)
	fmt.Fprintf(w, "id\t%04x:%04x  revision %02x\n", r.Vendor, r.Device, r.Revision)
	fmt.Fprintf(w, "bar0\t%#x  %d KiB\n", r.BAR0, r.BAR0Size/1024)
	switch {
	case r.DevRevOK:
		fmt.Fprintf(w, "dev_rev_id\t%#08x  matches the BCM56860 at revision 02\n", r.DevRevID)
	case r.DevRevID != 0:
		// Worth failing loudly on: the chip answered, with the wrong identity.
		fmt.Fprintf(w, "dev_rev_id\t%#08x  UNEXPECTED, want %#08x\n", r.DevRevID, 0x0002b860)
	}
	w.Flush()

	if r.AllOnes {
		// Every read returning 0xffffffff is what the host bridge gives back
		// when nothing answers. Reporting it as data would be reporting the
		// absence of a chip as the presence of one.
		return fmt.Errorf("every word of BAR0 read back as 0xffffffff, which is what " +
			"a PCI read returns when nothing answers: the chip is on the bus but not " +
			"responding to MMIO")
	}

	fmt.Printf("\nBAR0 +0x000 .. +%#05x, first non-trivial words:\n", len(r.Words)*4)
	shown := 0
	for i, v := range r.Words {
		if v == 0 || v == 0xffffffff {
			continue
		}
		fmt.Printf("  +%#05x  %#08x\n", i*4, v)
		if shown++; shown >= 16 {
			fmt.Printf("  ... (%d more non-zero words)\n", countInteresting(r.Words)-shown)
			break
		}
	}
	if shown == 0 {
		fmt.Println("  (all zero -- the chip answers, but this window reads as zeroes)")
	}
	fmt.Printf("\nthe chip answers MMIO from a standalone boot.\n")
	return nil
}

func countInteresting(ws []uint32) int {
	n := 0
	for _, v := range ws {
		if v != 0 && v != 0xffffffff {
			n++
		}
	}
	return n
}

// showTransceivers reports which cages are populated.
//
// Read from the board controller, not the switch chip, so it works with the
// ASIC dark and owes nothing to the port map -- which is what makes it useful
// for establishing one. A cage with a module in it is a cage that should have
// link once the right logical port is pointed at it.
func showTransceivers(hal platformhal.HAL) error {
	t, ok := hal.(interface{ Transceivers() ([]scd.Cage, error) })
	if !ok {
		return fmt.Errorf("%w: this board cannot report its cages", platformhal.ErrUnsupported)
	}
	cages, err := t.Transceivers()
	if err != nil {
		return err
	}

	w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
	fmt.Fprintln(w, "cage\ttype\tstate\traw")
	var populated, empty, unknown int
	for _, c := range cages {
		switch c.State {
		case scd.PresenceEmpty:
			empty++
			continue
		case scd.PresenceUnknown:
			unknown++
		default:
			populated++
		}
		fmt.Fprintf(w, "%d\t%s\t%s\t%#08x\n", c.Index, c.Kind, c.State, c.Raw)
	}
	w.Flush()

	fmt.Printf("\n%d populated, %d empty, %d undetermined, of %d cages.\n",
		populated, empty, unknown, len(cages))

	if unknown > 0 {
		// Worth saying plainly rather than leaving the reader to notice.
		fmt.Println("\nThe undetermined cages read words this driver has no meaning for.\n" +
			"The three it knows were measured while the vendor OS was driving the\n" +
			"board controller, and it is not driving it now -- so these are most\n" +
			"likely the table in some other state rather than modules that are\n" +
			"present. Presence cannot be told from this until the words are\n" +
			"established for a board the vendor OS has not touched.")
	}
	return nil
}

// setCageTX turns a front-panel cage's laser on or off.
//
// The board controller gates it, not the switch chip, so this is the one thing
// no amount of correct datapath configuration can do. A cage left disabled
// produces a link that is up from this end and absent from the other.
func setCageTX(hal platformhal.HAL, args []string) error {
	t, ok := hal.(interface {
		SetTX(int, bool) (uint32, uint32, error)
	})
	if !ok {
		return fmt.Errorf("%w: this board cannot gate its transmitters", platformhal.ErrUnsupported)
	}
	if len(args) < 2 {
		return fmt.Errorf("usage: nosaic platform tx <cage> on|off")
	}
	cage, err := strconv.Atoi(args[0])
	if err != nil {
		return fmt.Errorf("%q is not a cage number", args[0])
	}
	var on bool
	switch args[1] {
	case "on":
		on = true
	case "off":
		on = false
	default:
		return fmt.Errorf("expected on or off, got %q", args[1])
	}

	before, after, err := t.SetTX(cage, on)
	if err != nil {
		return err
	}
	state := "off"
	if on {
		state = "on"
	}
	fmt.Printf("cage %d transmitter %s: %#08x -> %#08x\n", cage, state, before, after)
	return nil
}
