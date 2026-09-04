// Package health decides whether a freshly booted image is good enough to keep.
//
// This is what "confirms itself healthy" means in practice. A trial slot that
// is never confirmed rolls back, which is the safe default, so the question
// here is only ever "is there positive evidence this image works".
package health

import (
	"fmt"
	"io"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// Result is what the datapath reported.
type Result struct {
	Ports   int
	AdminUp int
	OperUp  int
}

// Check asks the datapath whether this image actually works.
//
// "It booted" is deliberately not the bar. An image that reaches userspace and
// does not forward is precisely the case rollback exists for, and confirming
// on "init ran" would commit it -- which is what the old self-test did, in the
// one environment where it ran at all.
//
// Erring toward rollback is deliberate. Declining a healthy image costs a
// reboot and returns to a known-good slot; committing a broken one strands a
// switch. An operator who disagrees can always commit by hand.
//
// What this will not do is judge an image on something the image does not
// control: links are only required when something asked for a port to be up,
// so a switch whose ports are all administratively down is not failed for
// having no traffic.
func Check(sw switchapi.Switch, w io.Writer) (Result, error) {
	var r Result

	ports, err := sw.Ports()
	if err != nil {
		return r, fmt.Errorf("the datapath did not answer: %w", err)
	}
	r.Ports = len(ports)
	if r.Ports == 0 {
		// The daemon is up and the chip is not. This is the shape a broken
		// datapath actually takes -- the socket answers because the daemon
		// started, and there is nothing behind it.
		return r, fmt.Errorf("the datapath is running but knows about no ports")
	}

	for _, p := range ports {
		st, err := sw.PortStatus(p.Name)
		if err != nil {
			return r, fmt.Errorf("the datapath could not report on port %s: %w", p.Name, err)
		}
		if st.AdminUp {
			r.AdminUp++
		}
		if st.OperUp {
			r.OperUp++
		}
	}
	fmt.Fprintf(w, "datapath answers: %d ports, %d configured up, %d actually up\n",
		r.Ports, r.AdminUp, r.OperUp)

	if r.AdminUp > 0 && r.OperUp == 0 {
		return r, fmt.Errorf("%d port(s) are configured up and none of them is up: "+
			"this image is not forwarding", r.AdminUp)
	}
	return r, nil
}
