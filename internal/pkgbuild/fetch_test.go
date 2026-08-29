package pkgbuild

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/recipe"
)

// A source that answers with the wrong bytes must fall through to a mirror.
//
// This is what the mirror list is for, and it did not work: the hash was
// checked after the loop over sources, so a successful download of bad content
// ended the build with mirrors untried. CI hit it intermittently on zlib while
// the same URL served correct content from elsewhere.
func TestABadPrimaryFallsThroughToAMirror(t *testing.T) {
	good := []byte("the real source archive")
	sum := sha256.Sum256(good)

	bad := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// A 200 with the wrong content: the case a network-level retry
		// cannot see.
		fmt.Fprint(w, "<html>404 not found, served with a 200</html>")
	}))
	defer bad.Close()
	ok := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write(good)
	}))
	defer ok.Close()

	root := t.TempDir()
	o := Options{Root: root, Log: os.Stderr}
	r := &recipe.Recipe{
		Name: "example", Version: "1",
		Source: &recipe.Source{
			URL:     bad.URL + "/example-1.tar.gz",
			SHA256:  hex.EncodeToString(sum[:]),
			Mirrors: []string{ok.URL + "/example-1.tar.gz"},
		},
	}

	path, err := fetch(o, r)
	if err != nil {
		t.Fatalf("a good mirror was available and the fetch still failed: %v", err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(good) {
		t.Errorf("fetched %q, want the mirror's content", got)
	}
}

// And a bad download must not be left behind: it is indistinguishable from a
// good one by name, so the next build would find it cached and fail without
// fetching anything.
func TestABadDownloadIsNotLeftCached(t *testing.T) {
	bad := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "wrong")
	}))
	defer bad.Close()

	root := t.TempDir()
	o := Options{Root: root, Log: os.Stderr}
	r := &recipe.Recipe{
		Name: "example", Version: "1",
		Source: &recipe.Source{
			URL:    bad.URL + "/example-1.tar.gz",
			SHA256: "0000000000000000000000000000000000000000000000000000000000000000",
		},
	}
	if _, err := fetch(o, r); err == nil {
		t.Fatal("a checksum mismatch with no mirror should fail")
	}
	if _, err := os.Stat(filepath.Join(root, "dl", "example-1.tar.gz")); err == nil {
		t.Error("the bad download was left in dl/, where the next build will trust it")
	}
}
