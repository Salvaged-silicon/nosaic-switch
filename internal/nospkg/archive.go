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
	"path"
	"sort"
	"time"
)

// Entry is one file to be packaged.
type Entry struct {
	// Dst is the absolute path this lands on in the image.
	Dst string
	// Src is where to read the content from. Ignored for symlinks.
	Src string
	// Mode is the permission bits.
	Mode uint32
	// UID and GID are the numeric owner. Zero means root, which is both the
	// default and the correct answer for almost everything.
	UID int
	GID int
	// Link, if set, makes this a symlink to that target.
	Link string
	// Dir, if true, makes this a directory.
	Dir bool
}

const (
	manifestMember = "manifest.json"
	payloadMember  = "data.tar.gz"
)

// Build writes a .nos package to w.
//
// Everything that could vary between two builds of the same input is pinned:
// entries are sorted by destination, ownership is zeroed, and every timestamp
// is epoch rather than the clock.
func Build(w io.Writer, m *Manifest, entries []Entry) error {
	if m.Name == "" || m.Version == "" {
		return errors.New("nospkg: name and version are required")
	}
	if m.Arch == "" {
		return errors.New("nospkg: arch is required (use ArchAny if there is no architecture-specific content)")
	}
	if m.License == "" {
		return errors.New("nospkg: license is required")
	}
	m.FormatVersion = FormatVersion
	if m.Type == "" {
		m.Type = TypeComponent
	}

	// Sorted by destination so that the order entries were discovered in
	// cannot leak into the archive.
	sorted := append([]Entry(nil), entries...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].Dst < sorted[j].Dst })

	stamp := time.Unix(m.Build.Epoch, 0).UTC()

	payload, files, err := buildPayload(sorted, stamp)
	if err != nil {
		return err
	}
	m.Files = files
	sum := sha256.Sum256(payload)
	m.PayloadSHA256 = hex.EncodeToString(sum[:])

	manifestJSON, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return fmt.Errorf("nospkg: encoding manifest: %w", err)
	}
	manifestJSON = append(manifestJSON, '\n')

	// The outer tar is uncompressed, and the manifest comes first, so a
	// reader can decide whether a package is installable without touching
	// the payload.
	tw := tar.NewWriter(w)
	if err := writeMember(tw, manifestMember, manifestJSON, 0o644, stamp); err != nil {
		return err
	}
	if err := writeMember(tw, payloadMember, payload, 0o644, stamp); err != nil {
		return err
	}
	return tw.Close()
}

func buildPayload(entries []Entry, stamp time.Time) ([]byte, []File, error) {
	var buf bytes.Buffer

	// Name and ModTime are cleared in the gzip header: including them would
	// stamp the build host and the wall clock into the output.
	zw, err := gzip.NewWriterLevel(&buf, gzip.BestCompression)
	if err != nil {
		return nil, nil, err
	}
	zw.Name = ""
	zw.ModTime = time.Time{}
	zw.OS = 255 // "unknown", rather than whichever OS happened to build it

	tw := tar.NewWriter(zw)
	files := make([]File, 0, len(entries))

	for _, e := range entries {
		if !path.IsAbs(e.Dst) {
			return nil, nil, fmt.Errorf("nospkg: %q is not an absolute path", e.Dst)
		}
		hdr := &tar.Header{
			Name:    e.Dst,
			Mode:    int64(e.Mode),
			ModTime: stamp,
			Uid:     e.UID,
			Gid:     e.GID,
			Uname:   "",
			Gname:   "",
		}

		var content []byte
		switch {
		case e.Dir:
			hdr.Typeflag = tar.TypeDir
			if e.Mode == 0 {
				hdr.Mode = 0o755
			}
		case e.Link != "":
			hdr.Typeflag = tar.TypeSymlink
			hdr.Linkname = e.Link
			hdr.Mode = 0o777
		default:
			content, err = os.ReadFile(e.Src)
			if err != nil {
				return nil, nil, fmt.Errorf("nospkg: reading %s: %w", e.Src, err)
			}
			hdr.Typeflag = tar.TypeReg
			hdr.Size = int64(len(content))
		}

		if err := tw.WriteHeader(hdr); err != nil {
			return nil, nil, err
		}
		if len(content) > 0 {
			if _, err := tw.Write(content); err != nil {
				return nil, nil, err
			}
		}

		f := File{Path: e.Dst, Mode: uint32(hdr.Mode), Link: e.Link,
			UID: e.UID, GID: e.GID}
		if hdr.Typeflag == tar.TypeReg {
			s := sha256.Sum256(content)
			f.SHA256 = hex.EncodeToString(s[:])
			f.Size = hdr.Size
		}
		files = append(files, f)
	}

	if err := tw.Close(); err != nil {
		return nil, nil, err
	}
	if err := zw.Close(); err != nil {
		return nil, nil, err
	}
	return buf.Bytes(), files, nil
}

func writeMember(tw *tar.Writer, name string, content []byte, mode int64, stamp time.Time) error {
	hdr := &tar.Header{
		Name:     name,
		Mode:     mode,
		Size:     int64(len(content)),
		ModTime:  stamp,
		Typeflag: tar.TypeReg,
	}
	if err := tw.WriteHeader(hdr); err != nil {
		return err
	}
	_, err := tw.Write(content)
	return err
}
