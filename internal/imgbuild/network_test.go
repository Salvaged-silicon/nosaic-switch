package imgbuild

import (
	"strings"
	"testing"
)

// Re-running the network script must be free for an interface already in the
// state the file asks for. The retry loop runs every few seconds for minutes
// while the datapath creates its ports, and the MAC path takes an interface
// DOWN -- so a non-idempotent pass carries the management interface down and up
// over a hundred times per boot. That is not theoretical: it cost this board
// its management network, NO-CARRIER, for the rest of the session.
func TestApplyNetworkSkipsInterfacesAlreadyCorrect(t *testing.T) {
	for _, want := range []string{
		"have_addr=",                // reads the address that is already there
		"have_mac=",                 // and the MAC
		"is_up=",                    // and whether it is already up
		`[ "$have_mac" != "$mac" ]`, // only sets the MAC when it differs
	} {
		if !strings.Contains(applyNetwork, want) {
			t.Errorf("apply-network.sh is missing %q; a retry pass would reconfigure "+
				"interfaces that are already correct, and flap the management link", want)
		}
	}

	// The down/up must not be reachable without the MAC-differs test.
	down := strings.Index(applyNetwork, `ip link set dev "$name" down`)
	guard := strings.Index(applyNetwork, `[ "$have_mac" != "$mac" ]`)
	if down < 0 || guard < 0 || guard > down {
		t.Error("the interface down is not guarded by the MAC-differs test")
	}
}

// The wait exists so front-panel addresses are applied at all: the ports do not
// exist until the datapath daemon has created them, minutes after boot.
func TestApplyNetworkWaitsForTheDatapath(t *testing.T) {
	for _, want := range []string{"WAIT_SECS", "waiting for:", "never appeared after"} {
		if !strings.Contains(applyNetwork, want) {
			t.Errorf("apply-network.sh is missing %q; front-panel addresses would be "+
				"skipped on every normal boot", want)
		}
	}
}
