package n3172tq

import (
	"encoding/json"
	"net"
	"path/filepath"
	"testing"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/proto"
)

// fakeNosd serves one asic.temp reply, or hangs if reply is nil.
func fakeNosd(t *testing.T, reply *proto.Response, hang bool) string {
	t.Helper()
	sock := filepath.Join(t.TempDir(), "nosd.sock")
	ln, err := net.Listen("unix", sock)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })

	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				var req proto.Request
				if err := json.NewDecoder(c).Decode(&req); err != nil {
					return
				}
				if hang {
					// Accept, read, and never answer: the wedged-daemon case.
					time.Sleep(30 * time.Second)
					return
				}
				_ = json.NewEncoder(c).Encode(reply)
			}(c)
		}
	}()
	return sock
}

// useSocket points the driver at a socket for one test and puts the package
// var back afterwards. Without the restore, a test that leaves a live fake
// behind adds an ASIC reading to every later test in this package -- and the
// board-sensor tests assert on the exact number of sensors, so the failure
// would land somewhere else and look unrelated.
func useSocket(t *testing.T, path string) {
	t.Helper()
	prev := nosdSocket
	nosdSocket = path
	t.Cleanup(func() { nosdSocket = prev })
}

func okReply(t *testing.T, mons []map[string]int) *proto.Response {
	t.Helper()
	b, err := json.Marshal(mons)
	if err != nil {
		t.Fatal(err)
	}
	return &proto.Response{OK: true, Result: b}
}

func TestASICTempTakesTheHottestMonitor(t *testing.T) {
	useSocket(t, fakeNosd(t, okReply(t, []map[string]int{
		{"Index": 0, "MilliC": 52000, "PeakMilliC": 60000},
		{"Index": 1, "MilliC": 58500, "PeakMilliC": 61000},
		{"Index": 2, "MilliC": 51000, "PeakMilliC": 59000},
	}), false))

	got, ok := asicTempMilliC()
	if !ok {
		t.Fatal("reading was not available")
	}
	// Averaging would give 53833 and hide the hot spot the reading exists
	// to catch.
	if got != 58500 {
		t.Errorf("got %d, want the hottest monitor 58500", got)
	}
}

// No datapath running is the ordinary state on a board that has just booted,
// not a fault.
func TestASICTempAbsentWithoutNosd(t *testing.T) {
	useSocket(t, filepath.Join(t.TempDir(), "definitely-not-there.sock"))
	if _, ok := asicTempMilliC(); ok {
		t.Error("reported a reading with no datapath running")
	}
}

// A chip with no monitors answers with an empty list. That is not an error
// and must not be reported as 0 °C.
func TestASICTempEmptyListIsNotZeroDegrees(t *testing.T) {
	useSocket(t, fakeNosd(t, okReply(t, []map[string]int{}), false))
	if _, ok := asicTempMilliC(); ok {
		t.Error("an empty monitor list was reported as a reading")
	}
}

// A monitor that has never been read reports zero. Zero degrees on the
// hottest part of the board would wind the fans DOWN.
func TestASICTempRejectsZero(t *testing.T) {
	useSocket(t, fakeNosd(t, okReply(t, []map[string]int{
		{"Index": 0, "MilliC": 0, "PeakMilliC": 0},
	}), false))
	if got, ok := asicTempMilliC(); ok {
		t.Errorf("zero was reported as a reading: %d", got)
	}
}

func TestASICTempErrorIsNotAReading(t *testing.T) {
	useSocket(t, fakeNosd(t, &proto.Response{
		OK: false, Error: "temperature monitors: Unavailable",
	}, false))
	if _, ok := asicTempMilliC(); ok {
		t.Error("an error response was reported as a reading")
	}
}

// The one that protects the fans: a datapath that accepts and then wedges
// must cost one reading, not the cooling loop.
func TestASICTempGivesUpOnAWedgedDatapath(t *testing.T) {
	useSocket(t, fakeNosd(t, nil, true))
	start := time.Now()
	if _, ok := asicTempMilliC(); ok {
		t.Error("a wedged datapath produced a reading")
	}
	if el := time.Since(start); el > asicTempTimeout+3*time.Second {
		t.Errorf("blocked for %s; the cooling loop would have stalled", el)
	}
}

// Temperatures() must still work with no datapath: the board sensors alone.
func TestTemperaturesWithoutTheDie(t *testing.T) {
	useSocket(t, filepath.Join(t.TempDir(), "absent.sock"))

	temps, err := open(t, fakeSys(t)).Temperatures()
	if err != nil {
		t.Fatalf("Temperatures: %v", err)
	}
	if _, ok := temps["ASIC"]; ok {
		t.Error("an ASIC reading appeared with no datapath running")
	}
	if len(temps) == 0 {
		t.Error("no board sensors reported")
	}
}

// And the die joins the board sensors when the datapath is there.
func TestTemperaturesIncludeTheDie(t *testing.T) {
	useSocket(t, fakeNosd(t, okReply(t, []map[string]int{
		{"Index": 0, "MilliC": 57000, "PeakMilliC": 60000},
	}), false))

	temps, err := open(t, fakeSys(t)).Temperatures()
	if err != nil {
		t.Fatalf("Temperatures: %v", err)
	}
	if temps["ASIC"] != 57000 {
		t.Errorf("ASIC = %d, want 57000; temps = %v", temps["ASIC"], temps)
	}
	// And it is the hottest, which is the whole point.
	for name, v := range temps {
		if name != "ASIC" && v >= temps["ASIC"] {
			t.Errorf("%s (%d) is not cooler than the die (%d)", name, v, temps["ASIC"])
		}
	}
}
