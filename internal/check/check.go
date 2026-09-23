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
	"regexp"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/arch"
	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/boot"
	"github.com/salvaged-silicon/nosaic-switch/internal/depsolve"
	"github.com/salvaged-silicon/nosaic-switch/internal/docsgen"
	"github.com/salvaged-silicon/nosaic-switch/internal/identity"
	"github.com/salvaged-silicon/nosaic-switch/internal/profile"
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

	if id, err := identity.Load(root); err != nil {
		res.errf("loading base/identity.yml: %v", err)
	} else {
		for _, e := range id.Validate() {
			res.errf("base/identity.yml: %s", e)
		}
	}

	profiles, err := profile.LoadAll(root)
	if err != nil {
		res.errf("loading profiles: %v", err)
	}
	known := map[string]bool{}
	for _, p := range profiles {
		known[p.Name] = true
		for _, e := range p.Validate() {
			res.errf("%s: %s", rel(root, p.Path), e)
		}
	}

	arches, err := arch.LoadAll(root)
	if err != nil {
		res.errf("loading architectures: %v", err)
	}
	checkArches(res, root, arches)
	checkBootTools(res, root)
	checkDocumentedTargets(res, root)

	recipes, err := recipe.LoadAll(root)
	if err != nil {
		res.errf("loading recipes: %v", err)
	}
	checkRecipes(res, recipes)

	boards, err := board.LoadAll(root)
	if err != nil {
		res.errf("loading boards: %v", err)
	}
	checkBoardDocs(res, root, boards)
	checkNoUnitIdentity(res, root, boards)
	for _, b := range boards {
		for _, e := range b.Validate(root) {
			res.errf("%s: %s", rel(root, b.Path), e)
		}
		// A board naming a bootloader with no backend cannot produce an
		// installable image, which is worth saying before a build discovers it.
		if b.Boot != "" {
			if _, err := boot.For(b.Boot); err != nil {
				res.errf("%s: %v", rel(root, b.Path), err)
			}
		}
		// A board naming a profile that does not exist is a board that cannot
		// be built, and saying so here costs nothing.
		if b.Profile != "" && len(known) > 0 && !known[b.Profile] {
			res.errf("%s: profile %q has no base/%s.yml", rel(root, b.Path), b.Profile, b.Profile)
		}
	}

	return res
}

// checkBootTools makes the builder container's package list answerable to the
// code rather than to memory. A backend that shells out to a tool the
// container does not install fails at image-build time with whatever confusing
// error that tool happens to produce -- which is how both zip and dtc were
// found, each after a red CI run.
// checkDocumentedTargets keeps the documentation honest about the commands it
// tells people to run. A guide naming a target that does not exist is worse
// than no guide: the reader assumes they have got something wrong, and the
// first thing they try fails.
// checkBoardDocs requires every board to carry the three pages a reader needs,
// and requires them to have been filled in. A board port that lands without
// them is one only its author can install.
func checkBoardDocs(res *Result, root string, boards []*board.Board) {
	for _, b := range boards {
		for _, page := range []string{"install", "build", "hardware"} {
			p := filepath.Join(root, "platform", b.ID, "docs", page+".md")
			body, err := os.ReadFile(p)
			if os.IsNotExist(err) {
				res.errf("board %q has no %s — copy platform/TEMPLATE/docs/%s.md",
					b.ID, rel(root, p), page)
				continue
			}
			if err != nil {
				res.errf("cannot read %s: %v", rel(root, p), err)
				continue
			}
			// The template marks itself, so a page copied and not written is
			// caught here rather than by the first person to follow it.
			if strings.Contains(string(body), "> Delete this line when the page is filled in.") {
				res.errf("%s is still the unfilled template", rel(root, p))
			}
		}
		readme := filepath.Join(root, "platform", b.ID, "README.md")
		if _, err := os.Stat(readme); os.IsNotExist(err) {
			res.errf("board %q has no README.md", b.ID)
		}
	}

	stale, err := docsgen.Stale(root, boards)
	if err != nil {
		res.errf("cannot check %s: %v", docsgen.Path, err)
		return
	}
	if stale {
		res.errf("%s or the switches table in %s is out of date — run: make docs",
			docsgen.Path, docsgen.READMEPath)
	}
}

// checkNoUnitIdentity keeps one physical switch's identity out of a public
// repository, and out of every image built from it.
//
// This is not a style rule. platform/<board>/config/network.conf and frr.conf
// are COPIED INTO THE IMAGE, so whatever they say is asserted by every switch
// that boots one. Three boards shipped a real management address, a real
// management MAC and a real OSPF router-id, which made a second unit built
// from this tree a duplicate of the lab's first one -- same address on the
// management LAN, same router-id in the same OSPF area, and an ARP entry
// upstream that flaps between two machines. It is also simply somebody's
// hardware, published.
//
// The split that fixes it already exists. A switch's own values belong in
// /mnt/data/config/ on the data partition, where they survive an upgrade, a
// rollback and a reinstall; a local copy may be kept in <name>.site.conf,
// which .gitignore excludes. What stays in the tree is the shape and the
// reasoning, as <name>.conf.example.
//
// Only config/ is examined. Documentation quotes captures from real hardware
// and has to -- docs/hardware.md is worthless if its register dumps are
// fictional -- so the line is drawn at files the build consumes.
func checkNoUnitIdentity(res *Result, root string, boards []*board.Board) {
	for _, b := range boards {
		for _, name := range []string{"network.conf", "frr.conf"} {
			p := filepath.Join(root, "platform", b.ID, "config", name)
			if _, err := os.Stat(p); err == nil {
				stem := strings.TrimSuffix(name, ".conf")
				res.errf("%s is copied into the image, so it must not name one "+
					"switch — move the real values to %s.site.conf (gitignored) "+
					"or /mnt/data/config/ on the switch, and keep the shape as "+
					"%s.example", rel(root, p), stem, name)
			}
		}
		// The examples are published, so they are held to the same bar.
		for _, name := range []string{"network.conf.example", "frr.conf.example"} {
			p := filepath.Join(root, "platform", b.ID, "config", name)
			body, err := os.ReadFile(p)
			if err != nil {
				continue
			}
			for _, m := range identityRE.FindAllString(string(body), -1) {
				if vendorConstant[strings.ToLower(m)] {
					continue // a property of the model, not of a unit
				}
				res.errf("%s still contains %q — an example must carry no real "+
					"address or MAC", rel(root, p), m)
			}
		}
	}
}

// RFC1918 addresses and MAC addresses: what a unit is identified by.
var identityRE = regexp.MustCompile(
	`\b(?:10|172|192)\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\b` +
		`|\b(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b`)

// Addresses that are a property of the MODEL and belong in an example: an
// unprogrammed NIC's default, and the multicast MACs a switch must match.
var vendorConstant = map[string]bool{
	"00:10:18:00:00:00": true, // Broadcom, unprogrammed
	"00:a0:c9:00:00:00": true, // Intel, unprogrammed
	"00:00:00:00:00:00": true,
	"ff:ff:ff:00:00:00": true,
	"01:00:5e:00:00:00": true, // IPv4 multicast
	"33:33:00:00:00:00": true, // IPv6 multicast
}

func checkDocumentedTargets(res *Result, root string) {
	mk, err := os.ReadFile(filepath.Join(root, "Makefile"))
	if os.IsNotExist(err) {
		return
	}
	if err != nil {
		res.errf("cannot read Makefile: %v", err)
		return
	}
	targets := map[string]bool{}
	for _, line := range strings.Split(string(mk), "\n") {
		if m := targetRe.FindStringSubmatch(line); m != nil {
			targets[m[1]] = true
		}
	}

	docs := []string{filepath.Join(root, "README.md")}
	if entries, err := os.ReadDir(filepath.Join(root, "docs")); err == nil {
		for _, e := range entries {
			if strings.HasSuffix(e.Name(), ".md") {
				docs = append(docs, filepath.Join(root, "docs", e.Name()))
			}
		}
	}
	for _, doc := range docs {
		b, err := os.ReadFile(doc)
		if err != nil {
			continue
		}
		for _, m := range makeRe.FindAllStringSubmatch(string(b), -1) {
			if !targets[m[1]] {
				res.errf("%s says `make %s`, but the Makefile has no such target",
					rel(root, doc), m[1])
			}
		}
	}
}

var (
	targetRe = regexp.MustCompile(`^([a-z][a-z0-9_-]*):`)
	// "make foo" at a line start or after a backtick, so prose like
	// "make sure" is not mistaken for a target.
	makeRe = regexp.MustCompile("(?m)(?:^|`)make ([a-z][a-z0-9-]*)")
)

func checkBootTools(res *Result, root string) {
	// tool name -> the Debian package that provides it, where they differ.
	pkgFor := map[string]string{
		"mkimage":   "u-boot-tools",
		"dtc":       "device-tree-compiler",
		"mkfs.vfat": "dosfstools",
		"mcopy":     "mtools",
		"mmd":       "mtools",
	}
	df := filepath.Join(root, "builder", "Dockerfile.build")
	b, err := os.ReadFile(df)
	if os.IsNotExist(err) {
		// Not this check's business. It compares the tools backends run
		// against the packages the container installs; with no Dockerfile
		// there is nothing to compare, and a repo missing it fails to build a
		// container long before this matters.
		return
	}
	if err != nil {
		res.errf("cannot read %s: %v", rel(root, df), err)
		return
	}
	text := string(b)
	for _, id := range boot.All() {
		be, err := boot.For(id)
		if err != nil {
			continue
		}
		for _, tool := range be.Tools() {
			pkg := tool
			if p, ok := pkgFor[tool]; ok {
				pkg = p
			}
			// Word-boundary match: a bare substring search reports success for
			// "zip" against "bzip2", which is a false confirmation of exactly
			// the thing being checked.
			if !hasWord(text, pkg) {
				res.errf("boot backend %q runs %s, but %s does not install %s",
					id, tool, rel(root, df), pkg)
			}
		}
	}
}

// hasWord reports whether text contains word delimited by non-package-name
// characters, so "zip" does not match inside "bzip2".
func hasWord(text, word string) bool {
	for i := 0; i+len(word) <= len(text); i++ {
		if text[i:i+len(word)] != word {
			continue
		}
		before := byte(' ')
		if i > 0 {
			before = text[i-1]
		}
		after := byte(' ')
		if i+len(word) < len(text) {
			after = text[i+len(word)]
		}
		if !isPkgChar(before) && !isPkgChar(after) {
			return true
		}
	}
	return false
}

func isPkgChar(c byte) bool {
	return c == '-' || c == '.' || c == '+' ||
		(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
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

	// Every dependency must parse, and must resolve to a real package or to a
	// virtual name some package provides. An unresolvable dependency is a
	// build that fails late, on someone else's machine.
	for _, r := range recipes {
		for _, d := range r.Depends {
			req, err := depsolve.ParseRequirement(d)
			if err != nil {
				res.errf("%s: %q has an unparseable dependency: %v", r.Path, r.Name, err)
				continue
			}
			if _, ok := seen[req.Name]; ok {
				continue
			}
			if _, ok := provided[req.Name]; ok {
				continue
			}
			res.errf("%s: %q depends on %q, which no recipe provides", r.Path, r.Name, req.Name)
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
