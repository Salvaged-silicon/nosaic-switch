package imgbuild

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
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

// The management VRF, run for real rather than grepped for.
//
// It needs a network namespace of its own and a kernel with VRF support, so it
// skips where either is missing. NOSAIC_TEST_BUSYBOX names a busybox to run it
// under -- the switch's own "ip" is busybox's, and it is not iproute2 -- and
// without one the host's sh and ip are used.
func TestApplyNetworkManagementVRF(t *testing.T) {
	if _, err := exec.LookPath("unshare"); err != nil {
		t.Skip("no unshare")
	}
	if err := exec.Command("unshare", "-rn", "sh", "-c",
		"ip link add v type vrf table 9").Run(); err != nil {
		t.Skip("no unprivileged network namespace with VRF support here")
	}

	dir := t.TempDir()
	script := filepath.Join(dir, "apply.sh")
	conf := filepath.Join(dir, "network.conf")
	if err := os.WriteFile(script, []byte(applyNetwork), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(conf, []byte(`vrf mgmt table 1001
iface eth0 10.22.1.5/24 vrf mgmt
iface eth0 2001:db8:22::5/64 vrf mgmt
route default via 10.22.1.1 dev eth0 vrf mgmt
route default via 2001:db8:22::1 vrf mgmt
route 192.0.2.0/24 via 10.22.1.1 vrf nosuch
iface swp1 10.0.0.1/31
route default via 10.0.0.0
`), 0o644); err != nil {
		t.Fatal(err)
	}

	sh := "sh"
	path := os.Getenv("PATH")
	if bb := os.Getenv("NOSAIC_TEST_BUSYBOX"); bb != "" {
		bin := filepath.Join(dir, "bin")
		if err := os.Mkdir(bin, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(bb, filepath.Join(bin, "ip")); err != nil {
			t.Fatal(err)
		}
		sh, path = bb+" sh", bin+":"+path
	}
	run := fmt.Sprintf(`set -e
ip link add eth0 type dummy
ip link add swp1 type dummy
%[1]s %[2]s > pass1
%[1]s %[2]s > pass2
ip addr del 10.22.1.5/24 dev eth0
%[1]s %[2]s > pass3
echo "== master"; ip -o link show eth0
echo "== main"; ip route show
echo "== main6"; ip -6 route show
echo "== mgmt"; ip route show table 1001
echo "== mgmt6"; ip -6 route show table 1001
echo "== addrs"; ip -o addr show dev eth0
echo "== accept"; cat /proc/sys/net/ipv4/tcp_l3mdev_accept
`, sh, script)
	cmd := exec.Command("unshare", "-rn", "sh", "-c", run)
	cmd.Dir = dir
	cmd.Env = append(os.Environ(), "PATH="+path, "NOSAIC_NET_CONF="+conf, "NOSAIC_NET_WAIT=5")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	state := string(out)
	section := func(name string) string {
		s := state[strings.Index(state, "== "+name+"\n"):]
		if i := strings.Index(s[3:], "== "); i >= 0 {
			s = s[:i+3]
		}
		return s
	}
	pass1, _ := os.ReadFile(filepath.Join(dir, "pass1"))
	pass2, _ := os.ReadFile(filepath.Join(dir, "pass2"))
	pass3, _ := os.ReadFile(filepath.Join(dir, "pass3"))

	if !strings.Contains(section("master"), "master mgmt") {
		t.Errorf("eth0 is not in the VRF:\n%s", section("master"))
	}
	for _, want := range []string{"default via 10.22.1.1", "10.22.1.0/24 dev eth0"} {
		if !strings.Contains(section("mgmt"), want) {
			t.Errorf("table 1001 is missing %q:\n%s", want, section("mgmt"))
		}
	}
	if !strings.Contains(section("mgmt6"), "default via 2001:db8:22::1") {
		t.Errorf("the IPv6 management default is not in table 1001:\n%s", section("mgmt6"))
	}
	// The point of the exercise: nothing of the management network in the
	// table the datapath mirrors into the chip.
	for _, s := range []string{section("main"), section("main6")} {
		if strings.Contains(s, "eth0") || strings.Contains(s, "10.22.1.") {
			t.Errorf("management routes leaked into the main table:\n%s", s)
		}
	}
	if !strings.Contains(section("main"), "default via 10.0.0.0 dev swp1") {
		t.Errorf("the front-panel default route is missing:\n%s", section("main"))
	}
	// Joining a VRF cycles the interface, and a static IPv6 address does not
	// survive a down. Both must be there.
	for _, want := range []string{"10.22.1.5/24", "2001:db8:22::5/64"} {
		if !strings.Contains(section("addrs"), want) {
			t.Errorf("eth0 lost %s joining the VRF:\n%s", want, section("addrs"))
		}
	}
	if !strings.Contains(section("accept"), "1") {
		t.Error("tcp_l3mdev_accept is off: sshd would not answer on the management port")
	}
	if !strings.Contains(string(pass1), "no 'vrf nosuch table <N>' line") {
		t.Errorf("a route naming an undeclared VRF was not reported:\n%s", pass1)
	}
	// Second pass: everything is already right, so nothing is touched. A
	// re-enslave here would flap the management port on every reconcile.
	for _, line := range strings.Split(strings.TrimSpace(string(pass2)), "\n") {
		if strings.Contains(line, "vrf mgmt") || strings.Contains(line, "eth0") {
			t.Errorf("a second pass reconfigured something already correct: %q", line)
		}
	}
	// Third pass, after the address was lost and the VRF membership was not:
	// the address comes back, and eth0 is not put through the VRF again. The
	// re-join cycles the link, so a reconcile that did it would take the
	// management port down to repair it.
	if !strings.Contains(string(pass3), "eth0 10.22.1.5/24") {
		t.Errorf("the lost management address was not restored:\n%s", pass3)
	}
	if strings.Contains(string(pass3), "eth0 vrf mgmt") {
		t.Errorf("repairing an address re-joined eth0 to the VRF:\n%s", pass3)
	}
}
