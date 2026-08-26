// Package check validates the repository against the invariants NOSaic's
// design depends on.
//
// This runs in CI on every commit. Its job is to make the rules in
// docs/DESIGN.md mechanical rather than remembered — particularly the
// licensing gate, which cannot be retrofitted once vendor code is in the tree.
package check

import (
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/salvaged-silicon/nosaic-switch/internal/arch"
	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/recipe"
)

// requiredDirs are the structural seams. Each is an axis of the design; a
// missing one means something has been reorganised without updating the plan.
var requiredDirs = []string{
	"arch",      // per-CPU toolchain metadata
	"base",      // profile manifests: full / slim / minimal
	"boot",      // bootloader + installer backends
	"bootstrap", // crosstool-NG configs
	"builder",   // hermetic build containers
	"cmd",       // the nosaic binary
	"docs",
	"internal",
	"platform", // board ports
	"recipes",  // package recipes
}

// Result accumulates findings across the whole repository.
type Result struct {
	Errors   []string
	Warnings []string
}

func (r *Result) errf(f string, a ...any)  { r.Errors = append(r.Errors, fmt.Sprintf(f, a...)) }
func (r *Result) warnf(f string, a ...any) { r.Warnings = append(r.Warnings, fmt.Sprintf(f, a...)) }

// OK reports whether the repository passes.
func (r *Result) OK() bool { return len(r.Errors) == 0 }

// Run checks the repository rooted at root.
func Run(root string) *Result {
	res := &Result{}

	for _, d := range requiredDirs {
		if fi, err := os.Stat(filepath.Join(root, d)); err != nil || !fi.IsDir() {
			res.errf("missing required directory: %s/", d)
		}
	}

	// platform/TEMPLATE is contributor infrastructure. Without it, the first
	// person to add a board has nothing to copy and invents their own layout —
	// which is how a tree ends up with three boards that share no shape.
	if fi, err := os.Stat(filepath.Join(root, "platform", "TEMPLATE")); err != nil || !fi.IsDir() {
		res.errf("missing platform/TEMPLATE/ — a board port needs a scaffold to copy")
	}

	arches, err := arch.LoadAll(root)
	if err != nil {
		res.errf("loading architectures: %v", err)
	}
	checkArches(res, root, arches)

	recipes, err := recipe.LoadAll(root)
	if err != nil {
		res.errf("loading recipes: %v", err)
	}
	checkRecipes(res, recipes)

	boards, err := board.LoadAll(root)
	if err != nil {
		res.errf("loading boards: %v", err)
	}
	for _, b := range boards {
		for _, e := range b.Validate(root) {
			res.errf("%s: %s", rel(root, b.Path), e)
		}
	}

	return res
}

func checkArches(res *Result, root string, arches []*arch.Arch) {
	seenTriple := map[string]string{}

	for _, a := range arches {
		for _, e := range a.Validate() {
			res.errf("%s: %s", rel(root, a.Path), e)
		}

		if a.Triple != "" {
			if prev, dup := seenTriple[a.Triple]; dup {
				res.errf("%s: triple %q is already used by %s", rel(root, a.Path), a.Triple, prev)
			}
			seenTriple[a.Triple] = a.ID
		}

		// An architecture that is meant to be built needs a committed
		// defconfig. Without one the toolchain is not reproducible: it would
		// depend on whatever the upstream sample happened to be on the day
		// somebody ran the seed.
		if a.Status == "planned" {
			continue
		}
		cfg := filepath.Join(root, "bootstrap", "configs", a.ID+".defconfig")
		if _, err := os.Stat(cfg); os.IsNotExist(err) {
			res.errf("%s: status %q but bootstrap/configs/%s.defconfig is missing (run: make toolchain-seed ARCH=%s)",
				rel(root, a.Path), a.Status, a.ID, a.ID)
		}
	}
}

func checkRecipes(res *Result, recipes []*recipe.Recipe) {
	seen := map[string]string{}
	provided := map[string][]string{}

	for _, r := range recipes {
		for _, e := range r.Validate() {
			res.errf("%s: %s", r.Path, e)
		}

		if r.Name != "" {
			if prev, dup := seen[r.Name]; dup {
				res.errf("%s: duplicate package name %q, already defined in %s", r.Path, r.Name, prev)
			}
			seen[r.Name] = r.Path
		}

		for _, p := range r.Provides {
			provided[p] = append(provided[p], r.Name)
		}

		// Not an error: a non-redistributable component is legitimate, it just
		// cannot appear in a published image. Surfacing it keeps that visible
		// rather than buried in a manifest nobody reads.
		if r.Redistributable != nil && !*r.Redistributable {
			res.warnf("%s: %q is not redistributable — it cannot ship in a published image", r.Path, r.Name)
		}
	}

	// Every dependency must resolve to a real package or to a virtual name
	// some package provides. An unresolvable dependency is a build that fails
	// late, on someone else's machine.
	for _, r := range recipes {
		for _, d := range r.Depends {
			if _, ok := seen[d]; ok {
				continue
			}
			if _, ok := provided[d]; ok {
				continue
			}
			res.errf("%s: %q depends on %q, which no recipe provides", r.Path, r.Name, d)
		}
	}
}

// Report writes a human-readable summary and returns true if the repo passes.
func (r *Result) Report(w io.Writer) bool {
	for _, warn := range r.Warnings {
		fmt.Fprintf(w, "warning: %s\n", warn)
	}
	for _, e := range r.Errors {
		fmt.Fprintf(w, "error: %s\n", e)
	}
	switch {
	case len(r.Errors) > 0:
		fmt.Fprintf(w, "\nFAIL — %d error(s), %d warning(s)\n", len(r.Errors), len(r.Warnings))
		return false
	case len(r.Warnings) > 0:
		fmt.Fprintf(w, "\nOK — %d warning(s)\n", len(r.Warnings))
	default:
		fmt.Fprintln(w, "OK")
	}
	return true
}

func rel(root, p string) string {
	if r, err := filepath.Rel(root, p); err == nil {
		return r
	}
	return p
}
