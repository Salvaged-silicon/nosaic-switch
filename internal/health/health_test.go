package health

import (
	"errors"
	"io"
	"strings"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/mem"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// Each of these wraps a working switch and breaks one thing, which is the
// shape a bad image actually takes: everything else still works.
type deadDatapath struct{ *mem.Switch }

func (d deadDatapath) Ports() ([]switchapi.Port, error) {
	return nil, errors.New("dial unix /run/nosd.sock: connect: connection refused")
}

type noPorts struct{ *mem.Switch }

func (n noPorts) Ports() ([]switchapi.Port, error) { return nil, nil }

type noLink struct{ *mem.Switch }

func (n noLink) PortStatus(name string) (switchapi.PortStatus, error) {
	st, err := n.Switch.PortStatus(name)
	st.OperUp = false
	return st, err
}

// A switch as a running one actually is: ports exist and are configured up.
// mem's ports default to admin-down, which is a switch nobody has configured
// rather than a working one.
func working(t *testing.T) *mem.Switch {
	t.Helper()
	sw := mem.New(mem.Config{Ports: 4})
	if err := sw.Start(); err != nil {
		t.Fatal(err)
	}
	ports, err := sw.Ports()
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range ports {
		if err := sw.SetPortAdmin(p.Name, true); err != nil {
			t.Fatal(err)
		}
	}
	return sw
}

func TestAWorkingDatapathIsHealthy(t *testing.T) {
	r, err := Check(working(t), io.Discard)
	if err != nil {
		t.Fatalf("a working switch was judged unhealthy: %v", err)
	}
	if r.Ports != 4 || r.OperUp == 0 {
		t.Errorf("result %+v does not describe a working switch", r)
	}
}

// The realistic broken image: it boots, ssh answers, and the datapath is dead.
func TestADatapathThatDoesNotAnswerIsNotHealthy(t *testing.T) {
	if _, err := Check(deadDatapath{working(t)}, io.Discard); err == nil {
		t.Fatal("an image whose datapath does not answer was judged healthy")
	}
}

// The daemon started and the chip did not.
func TestADatapathWithNoPortsIsNotHealthy(t *testing.T) {
	_, err := Check(noPorts{working(t)}, io.Discard)
	if err == nil {
		t.Fatal("a datapath reporting no ports was judged healthy")
	}
	if !strings.Contains(err.Error(), "no ports") {
		t.Errorf("unhelpful message for the no-ports case: %v", err)
	}
}

// Ports configured up and none of them up is the definition of not forwarding.
func TestPortsConfiguredUpButNoneUpIsNotHealthy(t *testing.T) {
	if _, err := Check(noLink{working(t)}, io.Discard); err == nil {
		t.Fatal("a switch with no link on any configured port was judged healthy")
	}
}

// An image must not be failed for something it does not control. A switch
// whose ports are all administratively down has no traffic by request.
func TestAllPortsAdminDownIsNotAFailure(t *testing.T) {
	sw := working(t)
	ports, err := sw.Ports()
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range ports {
		if err := sw.SetPortAdmin(p.Name, false); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := Check(noLink{sw}, io.Discard); err != nil {
		t.Errorf("a switch with every port administratively down was failed: %v", err)
	}
}

func TestCheckSaysWhatItFound(t *testing.T) {
	var b strings.Builder
	if _, err := Check(working(t), &b); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(b.String(), "4 ports") {
		t.Errorf("the check does not report what it saw: %q", b.String())
	}
}
