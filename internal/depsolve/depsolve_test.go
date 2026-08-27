package depsolve

import (
	"strings"
	"testing"
)

func names(ps []Pkg) []string {
	out := make([]string, len(ps))
	for i, p := range ps {
		out[i] = p.Name
	}
	return out
}

func before(t *testing.T, order []Pkg, a, b string) {
	t.Helper()
	ai, bi := -1, -1
	for i, p := range order {
		switch p.Name {
		case a:
			ai = i
		case b:
			bi = i
		}
	}
	if ai < 0 || bi < 0 {
		t.Fatalf("expected both %s and %s in %v", a, b, names(order))
	}
	if ai > bi {
		t.Fatalf("%s must be installed before %s, got %v", a, b, names(order))
	}
}

func TestParseRequirement(t *testing.T) {
	cases := []struct{ in, name, op, ver string }{
		{"json-c", "json-c", "", ""},
		{"json-c >= 0.17", "json-c", ">=", "0.17"},
		{"json-c>=0.17", "json-c", ">=", "0.17"},
		{"frr == 10.2", "frr", "==", "10.2"},
		{"libyang != 2.0", "libyang", "!=", "2.0"},
		{"foo < 2", "foo", "<", "2"},
	}
	for _, c := range cases {
		r, err := ParseRequirement(c.in)
		if err != nil {
			t.Fatalf("%q: %v", c.in, err)
		}
		if r.Name != c.name || r.Op != c.op || r.Version != c.ver {
			t.Errorf("%q -> %+v", c.in, r)
		}
	}
	for _, bad := range []string{"", ">= 1.0", "foo >=", "foo bar"} {
		if _, err := ParseRequirement(bad); err == nil {
			t.Errorf("%q should not parse", bad)
		}
	}
}

func TestSatisfied(t *testing.T) {
	r, _ := ParseRequirement("json-c >= 0.17")
	if !r.Satisfied("0.17") || !r.Satisfied("0.18") || !r.Satisfied("1.0") {
		t.Fatal(">= should accept equal and greater")
	}
	if r.Satisfied("0.16") {
		t.Fatal(">= accepted a lower version")
	}
	// The case a lexical comparison gets wrong: "0.9" > "0.17" as strings,
	// but json-c 0.17 is newer than 0.9.
	if r.Satisfied("0.9") {
		t.Fatal("0.9 < 0.17 numerically, so >= 0.17 must reject it")
	}
}

func TestTopologicalOrder(t *testing.T) {
	avail := []Pkg{
		{Name: "frr", Version: "10.2", Depends: []string{"json-c", "libyang"}},
		{Name: "libyang", Version: "2.1", Depends: []string{"json-c"}},
		{Name: "json-c", Version: "0.17"},
	}
	order, err := Resolve(avail, []string{"frr"})
	if err != nil {
		t.Fatalf("resolve: %v", err)
	}
	if len(order) != 3 {
		t.Fatalf("expected the full closure, got %v", names(order))
	}
	before(t, order, "json-c", "libyang")
	before(t, order, "libyang", "frr")
}

// The order must not depend on map iteration, or two builds of one image
// could install in different orders.
func TestOrderIsStable(t *testing.T) {
	avail := []Pkg{
		{Name: "top", Version: "1", Depends: []string{"a", "b", "c"}},
		{Name: "a", Version: "1"}, {Name: "b", Version: "1"}, {Name: "c", Version: "1"},
	}
	first, err := Resolve(avail, []string{"top"})
	if err != nil {
		t.Fatal(err)
	}
	for range 20 {
		next, err := Resolve(avail, []string{"top"})
		if err != nil {
			t.Fatal(err)
		}
		if strings.Join(names(next), ",") != strings.Join(names(first), ",") {
			t.Fatalf("order varies between runs: %v vs %v", names(first), names(next))
		}
	}
}

func TestUnsatisfiedConstraint(t *testing.T) {
	avail := []Pkg{
		{Name: "frr", Version: "10.2", Depends: []string{"json-c >= 0.17"}},
		{Name: "json-c", Version: "0.16"},
	}
	_, err := Resolve(avail, []string{"frr"})
	if err == nil || !strings.Contains(err.Error(), "0.16") {
		t.Fatalf("expected a version complaint, got %v", err)
	}
}

func TestMissingDependency(t *testing.T) {
	avail := []Pkg{{Name: "frr", Version: "10.2", Depends: []string{"libyang"}}}
	_, err := Resolve(avail, []string{"frr"})
	if err == nil || !strings.Contains(err.Error(), "no package provides") {
		t.Fatalf("expected a missing-dependency error, got %v", err)
	}
}

func TestCycleDetected(t *testing.T) {
	avail := []Pkg{
		{Name: "a", Version: "1", Depends: []string{"b"}},
		{Name: "b", Version: "1", Depends: []string{"a"}},
	}
	_, err := Resolve(avail, []string{"a"})
	if err == nil || !strings.Contains(err.Error(), "cycle") {
		t.Fatalf("expected a cycle error, got %v", err)
	}
}

// A dependency on a virtual name resolves to whichever package provides it —
// this is how one nosd unit file serves every ASIC.
func TestVirtualNameResolves(t *testing.T) {
	avail := []Pkg{
		{Name: "nosaic-cli", Version: "1", Depends: []string{"nosd"}},
		{Name: "nosd-td2", Version: "1", Provides: []string{"nosd"}, Conflicts: []string{"nosd"}},
	}
	order, err := Resolve(avail, []string{"nosaic-cli"})
	if err != nil {
		t.Fatalf("resolve: %v", err)
	}
	before(t, order, "nosd-td2", "nosaic-cli")
}

// Two datapath implementations must never both be selected: they would fight
// over one chip.
func TestTwoProvidersRejected(t *testing.T) {
	avail := []Pkg{
		{Name: "img", Version: "1", Depends: []string{"nosd-td2", "nosd-helix4"}},
		{Name: "nosd-td2", Version: "1", Provides: []string{"nosd"}, Conflicts: []string{"nosd"}},
		{Name: "nosd-helix4", Version: "1", Provides: []string{"nosd"}, Conflicts: []string{"nosd"}},
	}
	_, err := Resolve(avail, []string{"img"})
	if err == nil || !strings.Contains(err.Error(), "exclusive") {
		t.Fatalf("expected an exclusivity error, got %v", err)
	}
}

// An ambiguous virtual name must be an error, not a silent pick: which
// datapath a board runs follows from its ASIC, never from sort order.
func TestAmbiguousVirtualNameRejected(t *testing.T) {
	avail := []Pkg{
		{Name: "cli", Version: "1", Depends: []string{"nosd"}},
		{Name: "nosd-td2", Version: "1", Provides: []string{"nosd"}},
		{Name: "nosd-helix4", Version: "1", Provides: []string{"nosd"}},
	}
	_, err := Resolve(avail, []string{"cli"})
	if err == nil || !strings.Contains(err.Error(), "name one explicitly") {
		t.Fatalf("expected an ambiguity error, got %v", err)
	}
}

func TestDuplicatePackageNames(t *testing.T) {
	avail := []Pkg{{Name: "a", Version: "1"}, {Name: "a", Version: "2"}}
	if _, err := Resolve(avail, []string{"a"}); err == nil {
		t.Fatal("two packages with one name should be an error")
	}
}
