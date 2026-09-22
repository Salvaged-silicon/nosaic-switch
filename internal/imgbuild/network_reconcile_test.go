package imgbuild

import (
	"strings"
	"testing"
)

// The taps belong to the datapath daemon, which is supervised with
// restart:always. When it crashes the supervisor restarts it directly -- s6-rc
// is not involved -- so every tap is destroyed and recreated bare, the
// loopback address goes with them, and the boot-time oneshot that would put
// them back is still marked done. The switch then runs for days and silently
// stops routing at a moment nothing logged.
func TestTheNetworkScriptCanReconcile(t *testing.T) {
	for _, want := range []string{
		"RECONCILE_SECS=${NOSAIC_NET_RECONCILE:-0}",
		"apply_ifaces",
		"apply_routes",
	} {
		if !strings.Contains(applyNetwork, want) {
			t.Errorf("the script is missing %q", want)
		}
	}

	// The loop has to come after the one-shot convergence, or a board still
	// waiting for its front-panel ports never reaches it.
	done := strings.Index(applyNetwork, "say done")
	loop := strings.Index(applyNetwork, "RECONCILE_SECS\" -gt 0")
	if done < 0 || loop < 0 {
		t.Fatalf("cannot find the convergence end (%d) or the loop (%d)", done, loop)
	}
	if loop < done {
		t.Error("the reconcile loop runs before convergence finishes")
	}
}

// Unset must mean "converge once and exit". The boot-time service is a oneshot
// and s6-rc waits for it: a script that never returns would hang the boot
// behind a dependency that can never be satisfied.
func TestReconcileIsOffByDefault(t *testing.T) {
	if !strings.Contains(applyNetwork, ":-0}") {
		t.Error("NOSAIC_NET_RECONCILE does not default to 0, so the boot-time " +
			"oneshot would never return")
	}
}

// A healthy switch must not write to its log every interval. apply_ifaces
// skips anything already in the state the file asks for and apply_routes skips
// a route already installed, so a pass with nothing to do is silent -- and the
// loop must not undo that by redirecting their output away either, because
// then a restored address would be invisible too.
func TestAReconcilePassIsNeitherNoisyNorSilenced(t *testing.T) {
	i := strings.Index(applyNetwork, "RECONCILE_SECS\" -gt 0")
	if i < 0 {
		t.Fatal("no reconcile loop")
	}
	body := applyNetwork[i:]
	// Precisely the two calls, not a blanket ban on /dev/null: the guard on
	// the loop legitimately sends `[`'s own error there when the value is not
	// a number, and that is stderr, not anything say() writes.
	for _, call := range []string{"apply_ifaces", "apply_routes"} {
		j := strings.Index(body, call)
		if j < 0 {
			t.Fatalf("the reconcile loop does not call %s", call)
		}
		line := body[j:]
		if k := strings.Index(line, "\n"); k >= 0 {
			line = line[:k]
		}
		if strings.Contains(line, ">/dev/null") || strings.Contains(line, "> /dev/null") {
			t.Errorf("%s redirects its output away, which would hide an address "+
				"being restored -- the one thing worth logging here: %q", call, line)
		}
	}
}
