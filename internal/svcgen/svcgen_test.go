package svcgen

import (
	"strings"
	"testing"
)

func gen(t *testing.T, b Backend, s Service) map[string]string {
	t.Helper()
	files, err := b.Generate(s)
	if err != nil {
		t.Fatalf("%s: %v", b.ID(), err)
	}
	out := map[string]string{}
	for _, f := range files {
		out[f.Path] = f.Content
	}
	return out
}

var zebra = Service{
	Name:  "zebra",
	Exec:  "/usr/sbin/zebra -f /etc/frr/zebra.conf",
	After: []string{"network", "nosd"},
}

func TestSystemdUnit(t *testing.T) {
	files := gen(t, systemd{}, zebra)
	unit, ok := files["/usr/lib/systemd/system/zebra.service"]
	if !ok {
		t.Fatalf("no unit generated; got %v", keys(files))
	}
	for _, want := range []string{
		"[Unit]", "[Service]", "[Install]",
		"ExecStart=/usr/sbin/zebra -f /etc/frr/zebra.conf",
		"WantedBy=multi-user.target",
	} {
		if !strings.Contains(unit, want) {
			t.Errorf("unit missing %q:\n%s", want, unit)
		}
	}
	// A bare name becomes a unit; a well-known one becomes a target.
	if !strings.Contains(unit, "After=network.target nosd.service") {
		t.Errorf("After not rendered as unit names:\n%s", unit)
	}
}

func TestS6Definition(t *testing.T) {
	files := gen(t, s6{}, zebra)
	for _, want := range []string{
		"/etc/s6-rc/source/zebra/type",
		"/etc/s6-rc/source/zebra/run",
		"/etc/s6-rc/source/zebra/dependencies.d/network",
		"/etc/s6-rc/source/zebra/dependencies.d/nosd",
	} {
		if _, ok := files[want]; !ok {
			t.Errorf("missing %s; got %v", want, keys(files))
		}
	}
	if got := files["/etc/s6-rc/source/zebra/type"]; got != "longrun\n" {
		t.Errorf("type = %q", got)
	}
	run := files["/etc/s6-rc/source/zebra/run"]
	if !strings.Contains(run, "exec /usr/sbin/zebra -f /etc/frr/zebra.conf") {
		t.Errorf("run script does not exec the service:\n%s", run)
	}
	if !strings.HasPrefix(run, "#!/bin/sh") {
		t.Errorf("run script has no interpreter line:\n%s", run)
	}
}

// The whole point: one declaration, both inits, and neither is hand-written.
func TestBothBackendsCoverTheSameService(t *testing.T) {
	for _, b := range All() {
		files, err := b.Generate(zebra)
		if err != nil {
			t.Fatalf("%s: %v", b.ID(), err)
		}
		if len(files) == 0 {
			t.Fatalf("%s generated nothing", b.ID())
		}
		joined := ""
		for _, f := range files {
			joined += f.Content
		}
		if !strings.Contains(joined, "/usr/sbin/zebra") {
			t.Errorf("%s never references the executable", b.ID())
		}
	}
}

// Generation must be deterministic, or packages stop being reproducible.
func TestDeterministic(t *testing.T) {
	s := Service{
		Name: "x", Exec: "/bin/x",
		After: []string{"c", "a", "b"},
		Wants: []string{"z", "y"},
	}
	for _, b := range All() {
		first := gen(t, b, s)
		for range 10 {
			next := gen(t, b, s)
			for k, v := range first {
				if next[k] != v {
					t.Fatalf("%s: output for %s varies between runs", b.ID(), k)
				}
			}
		}
	}
}

func TestValidation(t *testing.T) {
	bad := []struct {
		why string
		s   Service
	}{
		{"no name", Service{Exec: "/bin/x"}},
		{"no exec", Service{Name: "x"}},
		{"relative exec", Service{Name: "x", Exec: "x --flag"}},
		{"newline in exec", Service{Name: "x", Exec: "/bin/x\nrm -rf /"}},
		{"slash in name", Service{Name: "a/b", Exec: "/bin/x"}},
		{"bad restart", Service{Name: "x", Exec: "/bin/x", Restart: "sometimes"}},
	}
	for _, c := range bad {
		if err := c.s.Validate(); err == nil {
			t.Errorf("%s should be rejected", c.why)
		}
		for _, b := range All() {
			if _, err := b.Generate(c.s); err == nil {
				t.Errorf("%s: %s should not generate", b.ID(), c.why)
			}
		}
	}
}

func TestRestartMapping(t *testing.T) {
	always := gen(t, systemd{}, Service{Name: "x", Exec: "/bin/x", Restart: "always"})
	if !strings.Contains(always["/usr/lib/systemd/system/x.service"], "Restart=always") {
		t.Error("always not mapped")
	}
	never := gen(t, systemd{}, Service{Name: "x", Exec: "/bin/x", Restart: "never"})
	if !strings.Contains(never["/usr/lib/systemd/system/x.service"], "Restart=no") {
		t.Error("never should map to systemd's Restart=no")
	}
}

func TestUnknownInit(t *testing.T) {
	if _, err := For("upstart"); err == nil {
		t.Fatal("an unknown init system should be an error")
	}
	for _, id := range []string{"systemd", "s6"} {
		if b, err := For(id); err != nil || b.ID() != id {
			t.Errorf("For(%q) = %v, %v", id, b, err)
		}
	}
}

func keys(m map[string]string) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	return out
}

// A service that must not restart has to be a oneshot. s6-rc supervises a
// longrun and starts it again whenever it exits, so a service that configures
// something and returns runs forever -- which is what the network service did
// on the first switch it ran on, flooding the console it reports to.
func TestS6NeverRestartIsAOneshot(t *testing.T) {
	files, err := s6{}.Generate(Service{
		Name: "network-config", Exec: "/etc/nosaic/apply-network.sh", Restart: "never",
	})
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, f := range files {
		got[f.Path] = f.Content
	}
	if ty := got["/etc/s6-rc/source/network-config/type"]; ty != "oneshot\n" {
		t.Errorf("type is %q, want oneshot: a longrun is restarted every time it exits", ty)
	}
	if _, ok := got["/etc/s6-rc/source/network-config/run"]; ok {
		t.Error("a oneshot is described by up, not run")
	}
	up := got["/etc/s6-rc/source/network-config/up"]
	if !strings.Contains(up, "/etc/nosaic/apply-network.sh") {
		t.Errorf("up does not run the service: %q", up)
	}
	if strings.HasPrefix(up, "#!") {
		t.Error("s6-rc reads up with execline, so a shebang is wrong")
	}
}

// The ordinary case must stay a supervised longrun.
func TestS6DefaultIsALongrun(t *testing.T) {
	files, err := s6{}.Generate(Service{Name: "nosd", Exec: "/usr/sbin/nosd"})
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		if strings.HasSuffix(f.Path, "/type") && f.Content != "longrun\n" {
			t.Errorf("type is %q, want longrun", f.Content)
		}
	}
}

// execline groups on double quotes only, so a single-quoted region in an exec
// line is split on whitespace and the command runs as something else entirely.
// It is accepted and it does the wrong thing, which is the worst combination --
// and it works under systemd, so it breaks on one tier and not the other.
func TestSingleQuotesInExecAreRefused(t *testing.T) {
	s := Service{
		Name: "bind-something",
		Exec: `/bin/sh -c 'A=1; echo $A'`,
	}
	if err := s.Validate(); err == nil {
		t.Fatal("a single-quoted exec was accepted; execline would split it into words")
	}
	ok := Service{Name: "bind-something", Exec: `/bin/sh -c "A=1; echo $A"`}
	if err := ok.Validate(); err != nil {
		t.Fatalf("double quotes must remain usable: %v", err)
	}
}

// A verbose longrun logs through s6-log, with a size cap, and says so.
//
// Three things matter and they pull against each other. The output must not go
// to the console, because a datapath daemon carrying a vendor SDK writes enough
// to make a 9600 baud console useless. It must be bounded, because a plain file
// grows until the filesystem is gone -- on a board that RAM-boots that is RAM,
// and one of these daemons wrote 2 GB into a 1.9 GB tmpfs. And the daemon must
// keep its own process, because a shell pipeline between s6 and the daemon eats
// the stop signal, and stopping one of these cleanly is what keeps the chip out
// of a state only a board reset clears.
//
// s6-rc's producer/consumer pair is what satisfies all three, so this pins that
// shape rather than the commands.
func TestVerboseLongrunLogsThroughS6Log(t *testing.T) {
	svc := Service{
		Name:    "nosd",
		Exec:    "/usr/sbin/nosd",
		Restart: "always",
		Verbose: true,
	}
	files := gen(t, s6{}, svc)

	run := files["/etc/s6-rc/source/nosd/run"]
	if strings.Contains(run, "exec >/var/log") {
		t.Errorf("verbose longrun still redirects to a file, so the cap cannot apply:\n%s", run)
	}
	if !strings.Contains(run, "/dev/console") {
		t.Errorf("nothing on the console says where the output went:\n%s", run)
	}

	// The pipe, both halves. Either one alone is a service that never starts.
	if got := files["/etc/s6-rc/source/nosd/producer-for"]; got != "nosd-log\n" {
		t.Errorf("producer-for = %q", got)
	}
	if got := files["/etc/s6-rc/source/nosd-log/consumer-for"]; got != "nosd\n" {
		t.Errorf("consumer-for = %q", got)
	}
	if got := files["/etc/s6-rc/source/nosd-log/type"]; got != "longrun\n" {
		t.Errorf("logger type = %q; a oneshot logger would exit immediately", got)
	}

	// The cap is the whole point of the exercise.
	lg := files["/etc/s6-rc/source/nosd-log/run"]
	if !strings.Contains(lg, "s6-log") {
		t.Errorf("logger does not run s6-log:\n%s", lg)
	}
	if !strings.Contains(lg, "s1000000") || !strings.Contains(lg, "n3") {
		t.Errorf("logger has no size or rotation cap, so it can still fill the filesystem:\n%s", lg)
	}

	// A quiet service gets none of this: no logger, no redirect.
	svc.Verbose = false
	quiet := gen(t, s6{}, svc)
	if _, ok := quiet["/etc/s6-rc/source/nosd-log/run"]; ok {
		t.Error("non-verbose service got a logger it did not ask for")
	}
	if strings.Contains(quiet["/etc/s6-rc/source/nosd/run"], "/var/log") {
		t.Error("non-verbose service redirects somewhere")
	}
}

// A verbose oneshot has no logger to pipe into -- s6-rc only wires producers to
// consumers for longruns -- so it keeps redirecting to a file, and that file is
// still worth naming on the console.
func TestVerboseOneshotStillRedirects(t *testing.T) {
	svc := Service{
		Name:    "network-config",
		Exec:    "/etc/nosaic/apply-network.sh",
		Restart: "never",
		Verbose: true,
	}
	up := gen(t, s6{}, svc)["/etc/s6-rc/source/network-config/up"]
	if !strings.Contains(up, "/var/log/network-config.log") {
		t.Errorf("verbose oneshot does not capture its output:\n%s", up)
	}
}
