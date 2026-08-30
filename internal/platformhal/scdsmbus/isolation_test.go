// SPDX-License-Identifier: GPL-2.0-only

package scdsmbus

import (
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// This package must not import the rest of NOSaic.
//
// The isolation is what makes the licence boundary meaningful: this package is
// GPL-2.0 and everything around it is Apache-2.0, so the dependency has to run
// one way. An import added here would quietly pull GPL code into the licence
// story of whatever imported it, and nothing about the build would complain.
//
// Register access is injected through the Registers interface for exactly this
// reason. If that ever feels inconvenient, the answer is a wider interface,
// not an import.
func TestThisPackageImportsNothingFromNOSaic(t *testing.T) {
	entries, err := os.ReadDir(".")
	if err != nil {
		t.Fatal(err)
	}
	fset := token.NewFileSet()
	for _, e := range entries {
		if !strings.HasSuffix(e.Name(), ".go") {
			continue
		}
		f, err := parser.ParseFile(fset, filepath.Join(".", e.Name()), nil, parser.ImportsOnly)
		if err != nil {
			t.Fatalf("%s: %v", e.Name(), err)
		}
		for _, imp := range f.Imports {
			path := strings.Trim(imp.Path.Value, `"`)
			if strings.Contains(path, "nosaic-switch") {
				t.Errorf("%s imports %s. This package is GPL-2.0 and must depend "+
					"on nothing else in the tree; take what you need through the "+
					"Registers interface instead.", e.Name(), path)
			}
		}
	}
}

// The licence header must survive. A file added here without it, or with the
// wrong one, is the failure this whole arrangement exists to prevent.
func TestEveryFileCarriesTheGPLHeader(t *testing.T) {
	entries, err := os.ReadDir(".")
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		if !strings.HasSuffix(e.Name(), ".go") {
			continue
		}
		b, err := os.ReadFile(e.Name())
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(b), "SPDX-License-Identifier: GPL-2.0-only") {
			t.Errorf("%s has no GPL-2.0 SPDX header. Every file in this package "+
				"is GPL-2.0; if this one should not be, it belongs elsewhere.",
				e.Name())
		}
	}
}
