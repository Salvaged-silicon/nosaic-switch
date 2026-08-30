package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/platformhal"
	"github.com/salvaged-silicon/nosaic-switch/internal/thermal"
)

// thermalCmd wires a board's curve to the board-independent loop.
//
// Nothing here knows what controller is behind the fans. A board that has none
// says so through the capability model rather than through a HAL it cannot
// implement -- a fanless switch is a real thing.
func thermalCmd(hal platformhal.HAL, b *board.Board, args []string) error {
	c, ok := hal.(platformhal.Cooling)
	if !ok {
		return fmt.Errorf("%w: this board has no fan control", platformhal.ErrUnsupported)
	}

	curve := thermal.Curve{
		MinC:        b.Thermal.MinC,
		MaxC:        b.Thermal.MaxC,
		SlewDownMax: b.Thermal.SlewDownPercent,
		Interval:    time.Duration(b.Thermal.IntervalSeconds) * time.Second,
	}

	once := false
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--once":
			once = true
		case "--interval":
			if i+1 >= len(args) {
				return fmt.Errorf("--interval needs a number of seconds")
			}
			i++
			n, err := strconv.Atoi(args[i])
			if err != nil || n < 1 {
				return fmt.Errorf("%q is not a number of seconds", args[i])
			}
			curve.Interval = time.Duration(n) * time.Second
		default:
			return fmt.Errorf("unknown option %q", args[i])
		}
	}

	ctx, cancel := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer cancel()
	return thermal.Run(ctx, c, hal, curve, once, os.Stdout)
}
