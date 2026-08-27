// Package depsolve resolves package dependencies into an installation order.
//
// The problem here is deliberately smaller than a general package manager's.
// Every package in a NOSaic image comes from one build of one tree, so there
// are never two versions of anything to choose between: resolution is about
// completeness and ordering, not about satisfiability search. What it must
// catch is the set of mistakes that otherwise fail late — a dependency nothing
// provides, two implementations of one virtual name, a cycle, or a constraint
// the available version does not meet.
package depsolve

import (
	"fmt"
	"sort"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/vercmp"
)

// Pkg is the subset of a manifest that resolution needs.
type Pkg struct {
	Name      string
	Version   string
	Provides  []string
	Conflicts []string
	Depends   []string
}

// Requirement is a parsed dependency: a name, optionally constrained.
type Requirement struct {
	Name    string
	Op      string // "", ">=", ">", "<=", "<", "==", "!="
	Version string
}

// String renders a requirement the way it would be written in a recipe.
func (r Requirement) String() string {
	if r.Op == "" {
		return r.Name
	}
	return fmt.Sprintf("%s %s %s", r.Name, r.Op, r.Version)
}

var operators = []string{">=", "<=", "==", "!=", ">", "<"}

// ParseRequirement accepts "json-c" or "json-c >= 0.17", with or without
// spaces around the operator.
func ParseRequirement(s string) (Requirement, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return Requirement{}, fmt.Errorf("empty dependency")
	}
	// Longest operators first, so ">=" is not read as ">".
	for _, op := range operators {
		if i := strings.Index(s, op); i > 0 {
			name := strings.TrimSpace(s[:i])
			ver := strings.TrimSpace(s[i+len(op):])
			if name == "" {
				return Requirement{}, fmt.Errorf("%q: missing package name", s)
			}
			if ver == "" {
				return Requirement{}, fmt.Errorf("%q: %s without a version", s, op)
			}
			return Requirement{Name: name, Op: op, Version: ver}, nil
		}
	}
	if strings.ContainsAny(s, " \t") {
		return Requirement{}, fmt.Errorf("%q: expected a name, or a name with an operator such as >=", s)
	}
	return Requirement{Name: s}, nil
}

// Satisfied reports whether version v meets the requirement.
func (r Requirement) Satisfied(v string) bool {
	if r.Op == "" {
		return true
	}
	c := vercmp.Compare(v, r.Version)
	switch r.Op {
	case ">=":
		return c >= 0
	case ">":
		return c > 0
	case "<=":
		return c <= 0
	case "<":
		return c < 0
	case "==":
		return c == 0
	case "!=":
		return c != 0
	}
	return false
}

// Resolve returns the transitive closure of roots, in an order where every
// package appears after everything it depends on.
func Resolve(available []Pkg, roots []string) ([]Pkg, error) {
	byName := map[string]Pkg{}
	providers := map[string][]string{} // virtual name -> providing package names

	for _, p := range available {
		if prev, dup := byName[p.Name]; dup {
			return nil, fmt.Errorf("two packages are both named %q (versions %s and %s)",
				p.Name, prev.Version, p.Version)
		}
		byName[p.Name] = p
	}
	for _, p := range available {
		for _, v := range p.Provides {
			providers[v] = append(providers[v], p.Name)
		}
	}

	selected := map[string]bool{}
	var order []Pkg
	visiting := map[string]bool{}
	var stack []string

	var visit func(req Requirement, from string) error
	visit = func(req Requirement, from string) error {
		name, err := resolveName(req, byName, providers, from)
		if err != nil {
			return err
		}
		p := byName[name]

		if !req.Satisfied(p.Version) {
			return fmt.Errorf("%s requires %s, but the available %s is %s",
				describeFrom(from), req, name, p.Version)
		}
		if selected[name] {
			return nil
		}
		if visiting[name] {
			return fmt.Errorf("dependency cycle: %s -> %s",
				strings.Join(append(stack, name), " -> "), name)
		}

		visiting[name] = true
		stack = append(stack, name)
		// Sorted so the resulting order is stable across runs rather than
		// depending on map iteration.
		deps := append([]string(nil), p.Depends...)
		sort.Strings(deps)
		for _, d := range deps {
			dr, err := ParseRequirement(d)
			if err != nil {
				return fmt.Errorf("%s: %w", name, err)
			}
			if err := visit(dr, name); err != nil {
				return err
			}
		}
		stack = stack[:len(stack)-1]
		visiting[name] = false

		selected[name] = true
		order = append(order, p)
		return nil
	}

	for _, r := range roots {
		req, err := ParseRequirement(r)
		if err != nil {
			return nil, err
		}
		if err := visit(req, ""); err != nil {
			return nil, err
		}
	}

	if err := checkConflicts(order, providers); err != nil {
		return nil, err
	}
	return order, nil
}

// resolveName maps a requirement to a concrete package, through a virtual name
// if necessary.
func resolveName(req Requirement, byName map[string]Pkg, providers map[string][]string, from string) (string, error) {
	if _, ok := byName[req.Name]; ok {
		return req.Name, nil
	}
	switch ps := providers[req.Name]; len(ps) {
	case 1:
		return ps[0], nil
	case 0:
		return "", fmt.Errorf("%s depends on %q, which no package provides",
			describeFrom(from), req.Name)
	default:
		sort.Strings(ps)
		// Ambiguity is a build-time question, not a runtime guess: which
		// datapath a board runs is decided by its ASIC, not by whichever
		// provider happened to sort first.
		return "", fmt.Errorf("%s depends on %q, which %d packages provide (%s) — name one explicitly",
			describeFrom(from), req.Name, len(ps), strings.Join(ps, ", "))
	}
}

// checkConflicts rejects a selection containing packages that cannot coexist.
func checkConflicts(order []Pkg, providers map[string][]string) error {
	present := map[string]string{} // name or virtual name -> providing package
	for _, p := range order {
		present[p.Name] = p.Name
	}
	for _, p := range order {
		for _, v := range p.Provides {
			if prev, dup := present[v]; dup && prev != p.Name && prev != v {
				return fmt.Errorf("%s and %s both provide %q, and it is exclusive",
					prev, p.Name, v)
			}
			present[v] = p.Name
		}
	}
	for _, p := range order {
		for _, c := range p.Conflicts {
			holder, ok := present[c]
			if !ok || holder == p.Name {
				continue
			}
			return fmt.Errorf("%s conflicts with %q, provided by %s", p.Name, c, holder)
		}
	}
	return nil
}

func describeFrom(from string) string {
	if from == "" {
		return "the image"
	}
	return from
}
