package check

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// repo builds a throwaway tree with the required directories present, plus
// whatever recipes the test names.
func repo(t *testing.T, recipes map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for _, d := range requiredDirs {
		if err := os.MkdirAll(filepath.Join(root, d), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.MkdirAll(filepath.Join(root, "platform", "TEMPLATE"), 0o755); err != nil {
		t.Fatal(err)
	}
	for name, body := range recipes {
		dir := filepath.Join(root, "recipes", name)
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "recipe.yml"), []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

func hasErr(errs []string, substr string) bool {
	for _, e := range errs {
		if strings.Contains(e, substr) {
			return true
		}
	}
	return false
}

func TestEmptyRepoPasses(t *testing.T) {
	res := Run(repo(t, nil))
	if !res.OK() {
		t.Fatalf("a skeleton repo with no recipes should pass, got %v", res.Errors)
	}
}

func TestMissingRequiredDirFails(t *testing.T) {
	root := repo(t, nil)
	if err := os.RemoveAll(filepath.Join(root, "recipes")); err != nil {
		t.Fatal(err)
	}
	if res := Run(root); res.OK() {
		t.Fatal("a missing required directory should fail the check")
	}
}

// Contributor infrastructure is load-bearing: without a scaffold the first
// board port invents its own layout.
func TestMissingTemplateFails(t *testing.T) {
	root := repo(t, nil)
	if err := os.RemoveAll(filepath.Join(root, "platform", "TEMPLATE")); err != nil {
		t.Fatal(err)
	}
	res := Run(root)
	if !hasErr(res.Errors, "TEMPLATE") {
		t.Fatalf("missing TEMPLATE should be reported, got %v", res.Errors)
	}
}

func TestUnresolvableDependencyFails(t *testing.T) {
	res := Run(repo(t, map[string]string{
		"frr": `
name: frr
version: "10.2"
license: GPL-2.0-or-later
redistributable: true
depends: [libyang]
`}))
	if !hasErr(res.Errors, "which no recipe provides") {
		t.Fatalf("dangling dependency should fail, got %v", res.Errors)
	}
}

// A dependency on a virtual name is satisfied by whichever package provides
// it — that is the whole point of provides.
func TestVirtualNameSatisfiesDependency(t *testing.T) {
	res := Run(repo(t, map[string]string{
		"nosd-virt": `
name: nosd-virt
version: "0.1"
license: Apache-2.0
redistributable: true
provides: [nosd]
conflicts: [nosd]
`,
		"nosaic-cli": `
name: nosaic-cli
version: "0.1"
license: Apache-2.0
redistributable: true
depends: [nosd]
`}))
	if !res.OK() {
		t.Fatalf("a dependency on a provided virtual name should resolve, got %v", res.Errors)
	}
}

func TestDuplicatePackageNameFails(t *testing.T) {
	res := Run(repo(t, map[string]string{
		"a": "name: dup\nversion: \"1\"\nlicense: MIT\nredistributable: true\n",
		"b": "name: dup\nversion: \"2\"\nlicense: MIT\nredistributable: true\n",
	}))
	if !hasErr(res.Errors, "duplicate package name") {
		t.Fatalf("duplicate names should fail, got %v", res.Errors)
	}
}

// Non-redistributable is legitimate but must stay visible, so it warns rather
// than failing.
func TestNonRedistributableWarnsButPasses(t *testing.T) {
	res := Run(repo(t, map[string]string{
		"blob": `
name: blob
version: "1.0"
license: Vendor-Proprietary
redistributable: false
`}))
	if !res.OK() {
		t.Fatalf("non-redistributable should not fail the check, got %v", res.Errors)
	}
	if len(res.Warnings) == 0 {
		t.Fatal("non-redistributable should warn so it stays visible")
	}
}
