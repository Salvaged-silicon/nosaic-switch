package n3172tq

import (
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/proto"
)

// Reading the switch die, which is this board's hottest part and the one no
// i2c sensor can see.
//
// The vendor's own figures for this chassis: die 56 °C at idle against board
// diodes at 31, 37 and 38, with the die tripping at 100 and the diodes at 46
// to 60. A cooling loop regulating only on the diodes is regulating on the
// three coolest measurements in the box, which is why every board's band has
// had to carry margin standing in for this reading.
//
// ⚠ THE PATH IS THE DATAPATH DAEMON, AND THAT IS BOARD-SPECIFIC.
//
// Only the SDK can reach the die, over PCIe, so the only process that can ask
// is nosd. Another board may have the die on an i2c monitor or behind its own
// board controller; the HAL interface is the same either way and each board
// implements its own route to it. That is why this lives here and not in
// internal/thermal, which simply receives one more reading through
// Temperatures() and takes the hottest.

// nosdSocket is the datapath's query socket. A var so tests can point it
// somewhere that is not there, which is also the common case on a board whose
// datapath has not started yet.
var nosdSocket = proto.SocketPath

// asicTempTimeout bounds the whole exchange.
//
// This is called from the cooling loop. A datapath that accepts the
// connection and then wedges must cost one interval's reading, not the fan
// control -- the failure mode being protected against is fans frozen at their
// last duty with nothing in the log.
const asicTempTimeout = 2 * time.Second

// asicTempMilliC returns the hottest die monitor in millidegrees.
//
// ok is false whenever the reading is simply unavailable: no datapath
// running, a datapath too old to answer, or a chip with no monitors. All
// three are ordinary states rather than faults -- the board is still a
// working switch -- so the caller omits the sensor instead of failing.
func asicTempMilliC() (int, bool) {
	c, err := client.Dial(nosdSocket)
	if err != nil {
		return 0, false
	}
	defer c.Close()

	if err := c.SetDeadline(time.Now().Add(asicTempTimeout)); err != nil {
		return 0, false
	}
	mons, err := c.ASICTemperatures()
	if err != nil || len(mons) == 0 {
		return 0, false
	}

	// The chip reports several monitors at different points on the die. The
	// hottest is the one that matters for cooling; averaging them would hide
	// exactly the hot spot the reading exists to catch.
	hottest := mons[0].MilliC
	for _, m := range mons[1:] {
		if m.MilliC > hottest {
			hottest = m.MilliC
		}
	}

	// A monitor that has never been read reports zero, and zero degrees would
	// wind the fans down on the hottest part of the board.
	if hottest <= 0 {
		return 0, false
	}
	return hottest, true
}
