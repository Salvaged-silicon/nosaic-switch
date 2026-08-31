package imgbuild

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/nospkg"
)

// Where the generated attribution lands in the image.
const (
	noticePath = "/usr/share/nosaic/NOTICE"
	sbomPath   = "/usr/share/nosaic/sbom.json"
)

// component is one package as it appears in the attribution.
type component struct {
	Name            string `json:"name"`
	Version         string `json:"version"`
	License         string `json:"license"`
	Redistributable bool   `json:"redistributable"`
}

// sbom is the machine-readable half.
type sbom struct {
	Image string `json:"image"`
	Board string `json:"board"`
	Arch  string `json:"arch"`
	// Publishable is false when any component may not be redistributed. The
	// image is still built -- that is legitimate and useful locally -- but it
	// must not be published, and something has to be able to tell.
	Publishable bool        `json:"publishable"`
	Components  []component `json:"components"`
}

// writeAttribution generates the image's NOTICE and SBOM from the manifests of
// the packages that went into it.
//
// This is a licence obligation, not bookkeeping. Broadcom's OpenBCM licence is
// what makes an image containing the SDK distributable at all, and it requires
// that every distributed copy reproduce all proprietary notices -- removing or
// obscuring them is expressly forbidden. An image that ships the code and not
// the notice is the one case where the build has done something its licence
// does not permit.
//
// Generated from the package manifests rather than maintained by hand, for the
// same reason the switch catalog is: a hand-written list of what is in the
// image is a second place for the truth to live, and it goes stale silently.
//
// Returns whether the image may be published.
func writeAttribution(rootfs, image, board, arch string, comps []component, log io.Writer) (bool, error) {
	sort.Slice(comps, func(i, j int) bool { return comps[i].Name < comps[j].Name })

	publishable := true
	var refused []string
	for _, c := range comps {
		if !c.Redistributable {
			publishable = false
			refused = append(refused, c.Name)
		}
	}

	var b strings.Builder
	b.WriteString("NOSaic\n")
	b.WriteString("Copyright 2026 The NOSaic Authors\n\n")
	b.WriteString("This product includes software developed by the NOSaic project\n")
	b.WriteString("(https://github.com/salvaged-silicon/nosaic-switch).\n\n")
	fmt.Fprintf(&b, "Image:  %s\nBoard:  %s\nArch:   %s\n\n", image, board, arch)
	b.WriteString("This image contains the following components, each under its own\n")
	b.WriteString("license. NOSaic itself is Apache-2.0; the image as a whole is\n")
	b.WriteString("mixed-license.\n\n")

	width := 0
	for _, c := range comps {
		if n := len(c.Name + "-" + c.Version); n > width {
			width = n
		}
	}
	for _, c := range comps {
		nv := c.Name + "-" + c.Version
		lic := c.License
		if lic == "" {
			lic = "UNDECLARED"
		}
		fmt.Fprintf(&b, "  %-*s  %s", width, nv, lic)
		if !c.Redistributable {
			b.WriteString("  [NOT REDISTRIBUTABLE]")
		}
		b.WriteString("\n")
	}

	if !publishable {
		b.WriteString("\nThis image contains components that may not be redistributed:\n")
		for _, n := range refused {
			fmt.Fprintf(&b, "  %s\n", n)
		}
		b.WriteString("It is legitimate to build and run, and must not be published.\n")
	}

	if err := writeFile(rootfs, noticePath, b.String(), 0o644); err != nil {
		return publishable, err
	}

	doc := sbom{Image: image, Board: board, Arch: arch,
		Publishable: publishable, Components: comps}
	enc, err := json.MarshalIndent(doc, "", "  ")
	if err != nil {
		return publishable, err
	}
	if err := writeFile(rootfs, sbomPath, string(enc)+"\n", 0o644); err != nil {
		return publishable, err
	}

	fmt.Fprintf(log, "    attribution: %d components, %s\n", len(comps),
		map[bool]string{true: "publishable", false: "NOT publishable"}[publishable])
	if !publishable {
		fmt.Fprintf(log, "    not redistributable: %s\n", strings.Join(refused, ", "))
	}
	return publishable, nil
}

// readNotice returns the generated NOTICE from a composed tree, for tests and
// for anything that wants to check what an image will claim.
func readNotice(rootfs string) (string, error) {
	b, err := os.ReadFile(filepath.Join(rootfs, strings.TrimPrefix(noticePath, "/")))
	return string(b), err
}

// componentOf turns a package manifest into its attribution entry.
func componentOf(m *nospkg.Manifest) component {
	return component{
		Name:            m.Name,
		Version:         m.Version,
		License:         m.License,
		Redistributable: m.Redistributable,
	}
}
