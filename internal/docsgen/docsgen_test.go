package docsgen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
)

// boardWithDocs writes a board directory containing the named doc pages and
// returns a Board pointing at it.
func boardWithDocs(t *testing.T, pages ...string) *board.Board {
	t.Helper()
	dir := t.TempDir()
	docs := filepath.Join(dir, "docs")
	if err := os.MkdirAll(docs, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, p := range pages {
		if err := os.WriteFile(filepath.Join(docs, p+".md"), []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return &board.Board{ID: "b", Path: filepath.Join(dir, "board.yml")}
}

// A board that ships a page beyond the common three gets it linked. This is the
// whole reason the list is read from disk: a fixed list here meant a board
// could add documentation and have nothing point at it.
func TestPageLinksIncludesExtraPages(t *testing.T) {
	bd := boardWithDocs(t, "install", "build", "hardware", "architecture", "running")
	got := pageLinks(bd, "..")

	for _, want := range []string{"architecture", "running"} {
		if !strings.Contains(got, "["+want+"]") {
			t.Errorf("page %q is on disk but not linked: %s", want, got)
		}
	}
}

// The common pages keep a fixed order regardless of how the filesystem lists
// them, so the table does not reshuffle between machines.
func TestPageLinksOrder(t *testing.T) {
	bd := boardWithDocs(t, "hardware", "architecture", "build", "running", "install")
	got := pageLinks(bd, "..")

	want := []string{"install", "running", "build", "architecture", "hardware"}
	at := -1
	for _, w := range want {
		i := strings.Index(got, "["+w+"]")
		if i < 0 {
			t.Fatalf("missing %q in %s", w, got)
		}
		if i < at {
			t.Errorf("%q is out of order in %s", w, got)
		}
		at = i
	}
}

// Anything the order does not name still appears, after the ones it does.
func TestPageLinksUnknownPagesSortLast(t *testing.T) {
	bd := boardWithDocs(t, "install", "zebra", "alpha")
	got := pageLinks(bd, "..")

	if i, j := strings.Index(got, "[alpha]"), strings.Index(got, "[zebra]"); i < 0 || j < 0 || i > j {
		t.Errorf("unknown pages should follow in sorted order: %s", got)
	}
	if strings.Index(got, "[install]") > strings.Index(got, "[alpha]") {
		t.Errorf("named pages should come first: %s", got)
	}
}

// A board with no docs directory is reported as having none rather than
// linking every common page into a 404.
func TestPageLinksNoDocs(t *testing.T) {
	dir := t.TempDir()
	bd := &board.Board{ID: "b", Path: filepath.Join(dir, "board.yml")}
	if got := pageLinks(bd, ".."); got != "—" {
		t.Errorf("want an em dash for a board with no docs, got %q", got)
	}
}

// boardWithTools writes a board directory carrying the named generator
// scripts and returns a Board pointing at it.
func boardWithTools(t *testing.T, id string, tools ...string) *board.Board {
	t.Helper()
	dir := t.TempDir()
	if len(tools) > 0 {
		td := filepath.Join(dir, "tools")
		if err := os.MkdirAll(td, 0o755); err != nil {
			t.Fatal(err)
		}
		for _, x := range tools {
			if err := os.WriteFile(filepath.Join(td, x), []byte("#!/bin/sh\n"), 0o755); err != nil {
				t.Fatal(err)
			}
		}
	}
	return &board.Board{ID: id, Path: filepath.Join(dir, "board.yml")}
}

// A board that ships generators says so, and names them. Without this the
// catalog mentions the port map only by its absence, which reads as though the
// board is unsupported -- when the generator is precisely the supported part.
func TestGeneratorsAreListedForBoardsThatHaveThem(t *testing.T) {
	var b strings.Builder
	writeGenerators(&b, []*board.Board{boardWithTools(t, "somebox", "mkportmap.sh", "mkpolarity.sh")})
	got := b.String()

	for _, want := range []string{"somebox", "mkportmap.sh", "mkpolarity.sh", "not in\nthis repository"} {
		if !strings.Contains(got, want) {
			t.Errorf("the generator section does not mention %q:\n%s", want, got)
		}
	}
}

// A board with no generators contributes nothing, and with no boards at all the
// section is absent rather than present and empty.
func TestNoGeneratorSectionWhenNoBoardHasTools(t *testing.T) {
	var b strings.Builder
	writeGenerators(&b, []*board.Board{boardWithTools(t, "plain")})
	if got := b.String(); got != "" {
		t.Errorf("a board with no tools should add nothing, got:\n%s", got)
	}
}

// Listed in a stable order, so the page does not reshuffle between machines.
func TestGeneratorToolsAreSorted(t *testing.T) {
	var b strings.Builder
	writeGenerators(&b, []*board.Board{boardWithTools(t, "somebox", "zzz.sh", "aaa.sh")})
	got := b.String()
	if strings.Index(got, "aaa.sh") > strings.Index(got, "zzz.sh") {
		t.Errorf("tools should be sorted:\n%s", got)
	}
}
