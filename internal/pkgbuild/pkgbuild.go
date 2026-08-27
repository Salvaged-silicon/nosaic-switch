// Package pkgbuild turns a recipe into a .nos package.
//
// The flow is the same for every package, which is the point: there are no
// per-package build scripts, only data. Fetch a pinned source, verify it,
// apply a patch series, build it against the target's toolchain into a staging
// directory, and pack what landed there.
//
// Everything here runs inside the builder container, so the toolchain path and
// the repository root are the container's, not the host's.
package pkgbuild

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/salvaged-silicon/nosaic-switch/internal/arch"
	"github.com/salvaged-silicon/nosaic-switch/internal/nospkg"
	"github.com/salvaged-silicon/nosaic-switch/internal/recipe"
)

// Options controls one package build.
type Options struct {
	Root   string // repository root
	Recipe *recipe.Recipe
	Arch   *arch.Arch

	// Epoch is SOURCE_DATE_EPOCH. Zero means "derive from the recipe", which
	// keeps a build reproducible without requiring the caller to remember.
	Epoch int64

	// Jobs is the parallelism passed to make.
	Jobs int

	OutDir string
	Log    io.Writer
}

// Result describes what was produced.
type Result struct {
	Path     string
	Manifest *nospkg.Manifest
}

// Build runs the whole flow and writes a .nos package.
func Build(o Options) (*Result, error) {
	if o.Log == nil {
		o.Log = io.Discard
	}
	if o.Jobs <= 0 {
		o.Jobs = 1
	}
	r := o.Recipe
	if errs := r.Validate(); len(errs) > 0 {
		return nil, fmt.Errorf("recipe is invalid: %s", strings.Join(errs, "; "))
	}

	// A package with no architecture-specific content is tagged "any" so one
	// build serves every board rather than being rebuilt per CPU.
	pkgArch := nospkg.ArchAny
	if r.Build != nil && r.Build.System != "none" {
		pkgArch = o.Arch.ID
	}

	epoch := o.Epoch
	if epoch == 0 {
		epoch = defaultEpoch
	}

	work := filepath.Join(o.Root, ".cache", "pkg", r.Name)
	if err := os.RemoveAll(work); err != nil {
		return nil, err
	}
	stage := filepath.Join(work, "stage")
	if err := os.MkdirAll(stage, 0o755); err != nil {
		return nil, err
	}

	var srcDir string
	if r.Source != nil {
		tarball, err := fetch(o, r)
		if err != nil {
			return nil, err
		}
		if srcDir, err = extract(o, tarball, work); err != nil {
			return nil, err
		}
		if err := applyPatches(o, srcDir); err != nil {
			return nil, err
		}
	}

	if r.Build != nil && r.Build.System != "" && r.Build.System != "none" {
		if srcDir == "" {
			return nil, fmt.Errorf("build.system is %q but the recipe has no source", r.Build.System)
		}
		if err := runBuild(o, srcDir, stage); err != nil {
			return nil, err
		}
	}

	entries, err := collect(o, stage)
	if err != nil {
		return nil, err
	}

	svcEntries, err := generateServices(o, work)
	if err != nil {
		return nil, err
	}
	entries = append(entries, svcEntries...)

	// Checked before packaging, so a contaminated build never becomes a file
	// somebody could install.
	if err := verifyELF(o.Arch, entries); err != nil {
		return nil, err
	}

	if len(entries) == 0 {
		return nil, fmt.Errorf("%s: the build produced no files — check build.system and install:", r.Name)
	}

	m := &nospkg.Manifest{
		Name:               r.Name,
		Version:            r.Version,
		Summary:            r.Summary,
		Arch:               pkgArch,
		Type:               nospkg.TypeComponent,
		License:            r.License,
		Redistributable:    r.Redistributable != nil && *r.Redistributable,
		RuntimeInstallable: r.RuntimeInstallable,
		Provides:           r.Provides,
		Conflicts:          r.Conflicts,
		Depends:            r.Depends,
		Build: nospkg.BuildInfo{
			Epoch:  epoch,
			Triple: o.Arch.Triple,
		},
	}
	for _, s := range r.Services {
		m.Services = append(m.Services, nospkg.Service{
			Name: s.Name, Exec: s.Exec, After: s.After,
			Wants: s.Wants, Restart: s.Restart,
		})
	}

	if err := os.MkdirAll(o.OutDir, 0o755); err != nil {
		return nil, err
	}
	out := filepath.Join(o.OutDir, m.Filename())
	f, err := os.Create(out)
	if err != nil {
		return nil, err
	}
	if err := nospkg.Build(f, m, entries); err != nil {
		f.Close()
		return nil, err
	}
	if err := f.Close(); err != nil {
		return nil, err
	}
	fmt.Fprintf(o.Log, "==> %s (%d files)\n", out, len(m.Files))
	return &Result{Path: out, Manifest: m}, nil
}

// defaultEpoch is a fixed timestamp used when none is supplied. It is
// deliberately not the current time: a package built twice must be identical,
// and "now" is the one input guaranteed to differ.
const defaultEpoch = 1000000000 // 2001-09-09T01:46:40Z

func fetch(o Options, r *recipe.Recipe) (string, error) {
	dl := filepath.Join(o.Root, "dl")
	if err := os.MkdirAll(dl, 0o755); err != nil {
		return "", err
	}
	name := filepath.Base(r.Source.URL)
	path := filepath.Join(dl, name)

	if _, err := os.Stat(path); os.IsNotExist(err) {
		urls := append([]string{r.Source.URL}, r.Source.Mirrors...)
		var lastErr error
		for _, u := range urls {
			fmt.Fprintf(o.Log, "==> fetching %s\n", u)
			if err := downloadWithRetry(o, u, path); err != nil {
				fmt.Fprintf(o.Log, "    %v\n", err)
				lastErr = err
				continue
			}
			lastErr = nil
			break
		}
		if lastErr != nil {
			return "", fmt.Errorf("could not fetch %s from any source: %w", name, lastErr)
		}
	}

	// Verified on every build, not only after downloading: a cached file can
	// be corrupted, or replaced.
	sum, err := sha256File(path)
	if err != nil {
		return "", err
	}
	if sum != r.Source.SHA256 {
		return "", fmt.Errorf("%s: checksum mismatch\n  expected %s\n  got      %s",
			name, r.Source.SHA256, sum)
	}
	return path, nil
}

// downloadWithRetry fetches a URL, resuming rather than restarting.
//
// A transfer of a hundred-megabyte kernel tarball fails often enough on an
// ordinary connection that treating one interruption as fatal makes builds
// flaky for reasons that have nothing to do with the code. Resuming also means
// a retry costs only what was lost, not the whole file again.
func downloadWithRetry(o Options, url, dst string) error {
	const attempts = 4
	var err error
	for i := range attempts {
		if i > 0 {
			// Linear backoff: the failures worth retrying here are transient
			// resets and closed connections, not rate limits.
			time.Sleep(time.Duration(i*2) * time.Second)
			fmt.Fprintf(o.Log, "    retrying (%d of %d)\n", i+1, attempts)
		}
		err = downloadResume(url, dst+".part")
		if err == nil {
			return os.Rename(dst+".part", dst)
		}
	}
	return err
}

func downloadResume(url, tmp string) error {
	var have int64
	if fi, statErr := os.Stat(tmp); statErr == nil {
		have = fi.Size()
	}

	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	if have > 0 {
		req.Header.Set("Range", fmt.Sprintf("bytes=%d-", have))
	}

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	switch resp.StatusCode {
	case http.StatusOK:
		// The server ignored the range, so start again.
		have = 0
	case http.StatusPartialContent:
	case http.StatusRequestedRangeNotSatisfiable:
		// Already complete as far as the server is concerned.
		return nil
	default:
		return fmt.Errorf("%s: %s", url, resp.Status)
	}

	flags := os.O_CREATE | os.O_WRONLY
	if have > 0 {
		flags |= os.O_APPEND
	} else {
		flags |= os.O_TRUNC
	}
	f, err := os.OpenFile(tmp, flags, 0o644)
	if err != nil {
		return err
	}
	if _, err := io.Copy(f, resp.Body); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

func sha256File(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func extract(o Options, tarball, work string) (string, error) {
	src := filepath.Join(work, "src")
	if err := os.MkdirAll(src, 0o755); err != nil {
		return "", err
	}
	fmt.Fprintf(o.Log, "==> extracting %s\n", filepath.Base(tarball))
	// --no-same-owner: release tarballs carry the packager's uid, which means
	// nothing on either side of a container boundary.
	cmd := exec.Command("tar", "--no-same-owner", "-xf", tarball, "-C", src)
	if out, err := cmd.CombinedOutput(); err != nil {
		return "", fmt.Errorf("extracting %s: %v\n%s", tarball, err, out)
	}
	ents, err := os.ReadDir(src)
	if err != nil {
		return "", err
	}
	if len(ents) == 1 && ents[0].IsDir() {
		return filepath.Join(src, ents[0].Name()), nil
	}
	return src, nil
}

func applyPatches(o Options, srcDir string) error {
	r := o.Recipe
	if len(r.Patches) == 0 {
		return nil
	}
	dir := filepath.Dir(r.Path)
	for _, p := range r.Patches {
		full := filepath.Join(dir, p)
		fmt.Fprintf(o.Log, "==> applying %s\n", p)
		f, err := os.Open(full)
		if err != nil {
			return fmt.Errorf("patch %s: %w", p, err)
		}
		cmd := exec.Command("patch", "-p1", "--batch", "--forward")
		cmd.Dir = srcDir
		cmd.Stdin = f
		out, err := cmd.CombinedOutput()
		f.Close()
		if err != nil {
			return fmt.Errorf("applying %s: %v\n%s", p, err, out)
		}
	}
	return nil
}
