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
