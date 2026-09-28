package recipe

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
)

// Digest is a SHA-256 over everything in a recipe's directory that can change
// what it builds: recipe.yml, its patches, its config fragments. A package
// records it when it is built, and an image build compares it with the recipe
// on disk, so a package built from a recipe that has since changed is caught
// by content.
//
// Content, not file times: a checkout, a new worktree or a copied package
// directory all move file times without changing a recipe, and a check that
// fires on those is a check that gets overridden by habit.
//
// Prose (*.md) is left out. It cannot change a package, and a README edit that
// marks FRR stale is the same false alarm.
func Digest(dir string) (string, error) {
	var files []string
	err := filepath.Walk(dir, func(p string, fi os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if fi.Mode().IsRegular() && filepath.Ext(p) != ".md" {
			files = append(files, p)
		}
		return nil
	})
	if err != nil {
		return "", err
	}
	sort.Strings(files)

	h := sha256.New()
	for _, p := range files {
		rel, err := filepath.Rel(dir, p)
		if err != nil {
			return "", err
		}
		f, err := os.Open(p)
		if err != nil {
			return "", err
		}
		fi, err := f.Stat()
		if err != nil {
			f.Close()
			return "", err
		}
		// Each file's name and length go in ahead of its content, so renaming
		// a patch, or moving a line from one file to the next, still changes
		// the digest.
		fmt.Fprintf(h, "%s\x00%d\x00", filepath.ToSlash(rel), fi.Size())
		_, err = io.Copy(h, f)
		f.Close()
		if err != nil {
			return "", err
		}
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
