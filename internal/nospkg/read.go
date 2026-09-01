package nospkg

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// ErrNotNOS is returned for a file that is not a .nos package at all.
var ErrNotNOS = errors.New("nospkg: not a .nos package")

// ReadManifest reads only the manifest.
//
// The outer tar is uncompressed and the manifest is its first member, so this
// touches no payload bytes. That matters on a switch: deciding a package is
// for the wrong architecture should not cost a decompression.
func ReadManifest(r io.Reader) (*Manifest, error) {
	tr := tar.NewReader(r)
	hdr, err := tr.Next()
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrNotNOS, err)
	}
	if hdr.Name != manifestMember {
		return nil, fmt.Errorf("%w: first member is %q, expected %q", ErrNotNOS, hdr.Name, manifestMember)
	}
	var m Manifest
	if err := json.NewDecoder(tr).Decode(&m); err != nil {
		return nil, fmt.Errorf("nospkg: decoding manifest: %w", err)
	}
	if m.FormatVersion > FormatVersion {
		return nil, fmt.Errorf("nospkg: package is format version %d, this build understands %d",
			m.FormatVersion, FormatVersion)
	}
	return &m, nil
}

// ReadManifestFile reads the manifest from a package on disk.
func ReadManifestFile(path string) (*Manifest, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return ReadManifest(f)
}

// Verify re-derives every hash rather than trusting the manifest.
//
// A manifest that agrees with itself proves nothing: an attacker editing the
// payload would edit the recorded hashes too. What this checks is that the
// payload matches its recorded digest and that each file matches its own — so
// a single altered file is identified, not merely detected.
func Verify(r io.Reader) (*Manifest, error) {
	tr := tar.NewReader(r)

	hdr, err := tr.Next()
	if err != nil || hdr.Name != manifestMember {
		return nil, ErrNotNOS
	}
	var m Manifest
	if err := json.NewDecoder(tr).Decode(&m); err != nil {
		return nil, fmt.Errorf("nospkg: decoding manifest: %w", err)
	}

	hdr, err = tr.Next()
	if err != nil {
		return nil, fmt.Errorf("nospkg: reading payload: %w", err)
	}
	if hdr.Name != payloadMember {
		return nil, fmt.Errorf("%w: second member is %q, expected %q", ErrNotNOS, hdr.Name, payloadMember)
	}
	payload, err := io.ReadAll(tr)
	if err != nil {
		return nil, err
	}

	sum := sha256.Sum256(payload)
	if got := hex.EncodeToString(sum[:]); got != m.PayloadSHA256 {
		return nil, fmt.Errorf("nospkg: payload digest mismatch: recorded %s, computed %s",
			short(m.PayloadSHA256), short(got))
	}

	recorded := make(map[string]File, len(m.Files))
	for _, f := range m.Files {
		recorded[f.Path] = f
	}

	zr, err := gzip.NewReader(bytes.NewReader(payload))
	if err != nil {
		return nil, fmt.Errorf("nospkg: payload is not gzip: %w", err)
	}
	defer zr.Close()

	seen := 0
	ptr := tar.NewReader(zr)
	for {
		h, err := ptr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
		f, ok := recorded[h.Name]
		if !ok {
			return nil, fmt.Errorf("nospkg: payload contains %q, which the manifest does not list", h.Name)
		}
		seen++
		if h.Typeflag != tar.TypeReg {
			continue
		}
		content, err := io.ReadAll(ptr)
		if err != nil {
			return nil, err
		}
		s := sha256.Sum256(content)
		if got := hex.EncodeToString(s[:]); got != f.SHA256 {
			return nil, fmt.Errorf("nospkg: %s: digest mismatch (recorded %s, computed %s)",
				h.Name, short(f.SHA256), short(got))
		}
	}
	if seen != len(m.Files) {
		return nil, fmt.Errorf("nospkg: manifest lists %d files, payload contains %d", len(m.Files), seen)
	}
	return &m, nil
}

// VerifyFile verifies a package on disk.
func VerifyFile(path string) (*Manifest, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return Verify(f)
}

// CanInstall reports why a package may not be installed on a machine of the
// given architecture, or nil if it may.
//
// Installing a package built for another CPU produces a system that looks
// installed and does not run. Refusing is the only useful behaviour.
func (m *Manifest) CanInstall(arch string) error {
	if m.Arch != ArchAny && m.Arch != arch {
		return fmt.Errorf("package %s is built for %s, this machine is %s", m.Name, m.Arch, arch)
	}
	return nil
}

func short(h string) string {
	if len(h) > 12 {
		return h[:12] + "..."
	}
	return h
}

// Extract unpacks a package's payload into dst.
//
// Verification happens first and on the whole package: extracting a payload
// whose digest has not been checked would mean trusting a file that arrived
// from somewhere, which is the thing the format exists to avoid.
func Extract(path, dst string) (*Manifest, error) {
	m, err := VerifyFile(path)
	if err != nil {
		return nil, err
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	tr := tar.NewReader(f)
	if _, err := tr.Next(); err != nil { // manifest
		return nil, err
	}
	if _, err := tr.Next(); err != nil { // payload
		return nil, err
	}
	zr, err := gzip.NewReader(tr)
	if err != nil {
		return nil, err
	}
	defer zr.Close()

	ptr := tar.NewReader(zr)
	for {
		h, err := ptr.Next()
		if err == io.EOF {
			return m, nil
		}
		if err != nil {
			return nil, err
		}
		// Paths in a package are absolute by construction, and were validated
		// as such when it was built; joining defensively anyway means a
		// malformed package cannot write outside the target.
		clean := filepath.Join(dst, filepath.Clean("/"+h.Name))
		if !strings.HasPrefix(clean, filepath.Clean(dst)+string(os.PathSeparator)) {
			return nil, fmt.Errorf("nospkg: %q escapes the target directory", h.Name)
		}
		switch h.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(clean, 0o755); err != nil {
				return nil, err
			}
			// Same reasoning as for files, and it matters here for the sticky
			// bit: a /tmp extracted as 0755 is a /tmp no unprivileged process
			// can write to.
			if err := chmodRaw(clean, h.Mode); err != nil {
				return nil, err
			}
		case tar.TypeSymlink:
			if err := os.MkdirAll(filepath.Dir(clean), 0o755); err != nil {
				return nil, err
			}
			os.Remove(clean)
			if err := os.Symlink(h.Linkname, clean); err != nil {
				return nil, err
			}
		case tar.TypeReg:
			if err := os.MkdirAll(filepath.Dir(clean), 0o755); err != nil {
				return nil, err
			}
			// Unlink first. O_TRUNC on a path that is a symlink follows it and
			// truncates what it points AT, so a package installing a real file
			// where an earlier package left a symlink destroys the symlink's
			// target instead of replacing the link.
			//
			// This is not hypothetical. busybox installs /bin/ip as a symlink
			// to busybox; iproute2 is extracted afterwards and writes its own
			// /bin/ip, which landed on /bin/busybox. The image then had the ip
			// binary named busybox, and the initramfs -- which takes busybox
			// from the composed image -- panicked at boot with "Failed to
			// execute /init (error -2)", because /bin/sh pointed at a
			// dynamically linked binary whose loader was not there.
			//
			// The symlink case below already removes first. This one did not.
			if err := os.Remove(clean); err != nil && !os.IsNotExist(err) {
				return nil, err
			}
			out, err := os.OpenFile(clean, os.O_CREATE|os.O_EXCL|os.O_WRONLY, os.FileMode(h.Mode))
			if err != nil {
				return nil, err
			}
			if _, err := io.Copy(out, ptr); err != nil {
				out.Close()
				return nil, err
			}
			if err := out.Close(); err != nil {
				return nil, err
			}
			// The mode passed to OpenFile is not enough, for two reasons that
			// both end in a file with the wrong bits and no error.
			//
			// Go's os.FileMode is not a Unix mode: setuid is 1<<23 there, not
			// 0o4000, so a raw 0o4755 loses its setuid bit silently on the way
			// through. And open(2) applies the umask regardless. A doas or
			// sudo that is not setuid is a privilege path that exists, runs,
			// and does nothing -- which is the failure this whole change is
			// about.
			if err := chmodRaw(clean, h.Mode); err != nil {
				return nil, err
			}
			if err := chownEntry(clean, h.Uid, h.Gid); err != nil {
				return nil, err
			}
		}
	}
}

// chownEntry applies a package's declared ownership.
//
// Only when the package asked for something other than root: an unprivileged
// extraction (inspecting a package, a test) must not fail on the overwhelming
// majority of files that are root-owned anyway. When a recipe HAS named an
// owner, failing to apply it is an error rather than a warning -- a config
// file silently left as root:root is a daemon that starts and reads nothing,
// which is far harder to diagnose than a build that stops and says why.
func chownEntry(path string, uid, gid int) error {
	if uid == 0 && gid == 0 {
		return nil
	}
	if err := os.Lchown(path, uid, gid); err != nil {
		return fmt.Errorf("chown %s to %d:%d: %w", path, uid, gid, err)
	}
	return nil
}
