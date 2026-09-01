// Package nospkg implements the .nos package format.
//
// A .nos is an uncompressed outer tar containing exactly two members, in this
// order:
//
//	manifest.json   what the package is, what it needs, what it installs
//	data.tar.gz     the payload, at its install paths
//
// The outer tar is uncompressed so a reader can stream the manifest without
// decompressing the payload — useful on a switch deciding whether a package
// is even installable before spending CPU on it.
//
// Builds are reproducible: members are sorted, ownership is zeroed, and every
// timestamp comes from SOURCE_DATE_EPOCH rather than the clock. Building the
// same inputs twice produces byte-identical output, which is what makes "the
// image you built is the image I built" a checkable claim rather than a hope.
package nospkg

// FormatVersion is the on-disk format. Bump it only for a change a previous
// reader could not handle; adding an optional field is not such a change.
const FormatVersion = 1

// ArchAny marks a package with no architecture-specific content — scripts,
// configuration, documentation. It installs on any machine.
const ArchAny = "any"

// Package types.
const (
	// TypeComponent is an ordinary package layered onto a base.
	TypeComponent = "component"
	// TypeBase is a whole base system. Exactly one appears in an image, and
	// it is tagged by architecture only: a base contains no ASIC-specific
	// content, so one serves every board on that CPU.
	TypeBase = "base"
)

// File is one installed file, recorded so that installation can be verified
// afterwards and so tampering is detectable per-file rather than only for the
// payload as a whole.
type File struct {
	Path   string `json:"path"`   // absolute path in the image
	Mode   uint32 `json:"mode"`   // permission bits
	Size   int64  `json:"size"`   // bytes; 0 for symlinks and directories
	SHA256 string `json:"sha256"` // empty for symlinks and directories
	Link   string `json:"link,omitempty"`
	UID    int    `json:"uid,omitempty"` // 0 (root) unless a recipe said otherwise
	GID    int    `json:"gid,omitempty"`
}

// Service is an init-system-agnostic service definition. Recipes never write
// unit files: systemd units and s6/OpenRC definitions are both generated from
// this, so the minimal profile can ship a different init without every recipe
// that starts a daemon having to know.
type Service struct {
	Name    string   `json:"name"`
	Exec    string   `json:"exec"`
	After   []string `json:"after,omitempty"`
	Wants   []string `json:"wants,omitempty"`
	Restart string   `json:"restart,omitempty"`
}

// User is a system account a package needs in order to run.
//
// Declared by the package rather than known to the image builder: a daemon
// that drops privilege needs an account to drop to, and which account that is
// belongs next to the daemon. Putting it in the image builder instead means a
// central list that has to be edited every time a package is added, which is
// the pattern this project exists to avoid.
//
// UID and GID are fixed, not allocated. A package overlay installed onto a
// running switch has to produce the same ownership as the image did, and an
// allocator would not: the same package would get a different number depending
// on what else happened to be installed first, and files written before the
// upgrade would belong to somebody else after it.
type User struct {
	Name  string `json:"name"`
	UID   int    `json:"uid"`
	GID   int    `json:"gid"`
	Home  string `json:"home,omitempty"`
	Shell string `json:"shell,omitempty"`
}

// BuildInfo records how this package was produced.
type BuildInfo struct {
	// Epoch is SOURCE_DATE_EPOCH: the timestamp stamped into every archive
	// member. Never the wall clock, or builds would not be reproducible.
	Epoch int64 `json:"epoch"`

	Toolchain string `json:"toolchain,omitempty"` // e.g. "crosstool-ng 1.28.0"
	Triple    string `json:"triple,omitempty"`    // e.g. "powerpc-nosaic-linux-gnu"
	NOSaic    string `json:"nosaic,omitempty"`    // the version that built it
}

// Signature is reserved. Packages are SHA-256 hashed today, which detects
// corruption but not forgery — a hash is only as trustworthy as the channel
// that delivered it. The fields exist now so that adding signatures later is a
// feature rather than a format break that orphans every package built before
// it.
type Signature struct {
	Algorithm string `json:"algorithm,omitempty"`
	KeyID     string `json:"key_id,omitempty"`
	Value     string `json:"value,omitempty"`
}

// Manifest describes one package.
type Manifest struct {
	FormatVersion int    `json:"format_version"`
	Name          string `json:"name"`
	Version       string `json:"version"`
	Summary       string `json:"summary,omitempty"`

	// Arch is the CPU this package's contents are built for, or ArchAny.
	Arch string `json:"arch"`
	Type string `json:"type"`

	// License is an SPDX identifier or a named vendor license.
	License string `json:"license"`

	// Redistributable records whether this component may appear in a
	// published image. False is legitimate — vendor firmware, an SDK with a
	// restrictive license — and means the build will still produce an image
	// locally but refuse to publish one.
	Redistributable bool `json:"redistributable"`

	// RuntimeInstallable allows this package to be installed onto a running
	// switch rather than only composed into an image.
	RuntimeInstallable bool `json:"runtime_installable"`

	// Provides lists virtual names this package satisfies; Conflicts lists
	// names it cannot coexist with. A package providing a virtual name must
	// also conflict with it, so that two implementations of nosd cannot both
	// install and fight over one chip.
	Provides  []string `json:"provides,omitempty"`
	Conflicts []string `json:"conflicts,omitempty"`

	// Depends entries are a bare name, or a name with a constraint:
	// "json-c", "json-c >= 0.17".
	Depends []string `json:"depends,omitempty"`

	Services []Service `json:"services,omitempty"`
	Users    []User    `json:"users,omitempty"`
	Files    []File    `json:"files"`

	// PayloadSHA256 covers data.tar.gz as a whole.
	PayloadSHA256 string `json:"payload_sha256"`

	Build     BuildInfo `json:"build"`
	Signature Signature `json:"signature,omitempty"`
}

// Filename is the conventional name for this package on disk.
//
// Note there is no ASIC field: an ASIC-specific package carries the chip in
// its *name* (nosd-td2), so encoding it again in the filename would be
// redundant and would imply the two could disagree.
func (m *Manifest) Filename() string {
	return m.Name + "_" + m.Version + "_" + m.Arch + ".nos"
}
