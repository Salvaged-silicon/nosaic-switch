package main

import (
	"context"
	"fmt"
	"os"
	"sort"
	"strconv"
	"text/tabwriter"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
	_ "github.com/salvaged-silicon/nosaic-switch/internal/platformhal/scd"
)

const platformUsage = `usage: nosaic platform <command>

  status               what the board reports about itself
  release-asic         take the switch chip out of reset and wait for it
  watchdog status      whether the hardware watchdog is armed
  watchdog arm <ms>    arm it; the action is a power cycle
  watchdog disarm      stop it -- only with a console attached

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
