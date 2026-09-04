// Package config is a switch's configuration, as files.
//
// Two layers, and which is which is the whole design:
//
//	/etc/nosaic/*.conf        what the IMAGE shipped. Replaced by every
//	                          upgrade, and identical on every switch built
//	                          from that image.
//	/mnt/data/config/*.conf   what THIS SWITCH is. On the shared data
//	                          partition, so it survives an upgrade and also
//	                          survives a rollback.
//
// Loaded in that order with the last definition of a key winning, so a setting
// here overrides the image without editing the image. That is what lets a
// switch be configured without rebuilding it, and it is why Set writes only to
// the second directory.
//
// Everything is key=value text. A configuration you can read with cat, diff
// between two switches, and restore by copying a directory is worth more on
// this hardware than a database — and the same files are read by the C CLI on
// boards the Go toolchain cannot target, so the format is the contract.
package config

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

const (
	// ImageDir is what the image shipped; SiteDir is this switch's own.
	ImageDir = "/etc/nosaic"
	SiteDir  = "/mnt/data/config"

	// SiteFile is where Set writes. One file rather than per-subject files,
	// because a switch's own settings are few and having them in one place
	// makes "what is different about this box" a single cat.
	SiteFile = "local.conf"

	// ramBootMarker is written by the initramfs when there is no persistent
	// slot, so the running system knows its /mnt/data is a tmpfs.
	ramBootMarker = "/etc/nosaic/ramboot"

	// flashConfigDir is where a RAM-booted board keeps its configuration:
	// inside the bootloader's own flash filesystem, which survives a reboot
	// and an image replacement because that is where the images live too.
	flashConfigDir = "nosaic/config"
)

// flashCandidates are the partitions a bootloader's filesystem might be on.
// The initramfs probes the same list when it copies configuration in at boot;
// this is the other half of that, and the two have to agree.
var flashCandidates = []string{
	"/dev/mmcblk0p1", "/dev/sda1", "/dev/vda1", "/dev/mmcblk0p2",
}

// RAMBooted reports whether this switch has no persistent data partition.
//
// On such a board /mnt/data is a tmpfs: settings written there are correct
// until the next reboot and then gone, which is the worst kind of wrong for
// configuration. Set writes through to flash instead.
func RAMBooted() bool {
	_, err := os.Stat(ramBootMarker)
	return err == nil
}

// Setting is one effective value and where it came from.
type Setting struct {
	Name  string
	Value string
	// File is the file whose definition won.
	File string
	// Site is true when the winning definition came from this switch's own
	// configuration rather than from the image.
	Site bool
}

// Config is the effective configuration, in load order.
type Config struct {
	settings []Setting
	index    map[string]int
}

// Load reads the image's defaults and then this switch's own over the top.
//
// Directories that do not exist are not an error: a board with no
// configuration installed still has an image, and a switch nobody has
// configured yet has no site directory.
func Load() (*Config, error) {
	return LoadFrom(ImageDir, SiteDir)
}

// LoadFrom is Load with the directories named, for tests and for a build host
// that has neither.
func LoadFrom(imageDir, siteDir string) (*Config, error) {
	c := &Config{index: map[string]int{}}
	if err := c.loadDir(imageDir, false); err != nil {
		return nil, err
	}
	if err := c.loadDir(siteDir, true); err != nil {
		return nil, err
	}
	return c, nil
}

func (c *Config) loadDir(dir string, site bool) error {
	names, err := filepath.Glob(filepath.Join(dir, "*.conf"))
	if err != nil || names == nil {
		return nil
	}
	// Name order, so the layering inside a directory is stated by the
	// filenames rather than by whatever order the filesystem returns.
	sort.Strings(names)
	for _, n := range names {
		if err := c.loadFile(n, site); err != nil {
			return err
		}
	}
	return nil
}

func (c *Config) loadFile(path string, site bool) error {
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	defer f.Close()

	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		name, value, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		c.set(strings.TrimSpace(name), strings.TrimSpace(value), path, site)
	}
	return sc.Err()
}

// set replaces in place, so the slice holds the EFFECTIVE configuration and
// remembers which file won rather than every definition ever seen.
func (c *Config) set(name, value, file string, site bool) {
	if i, ok := c.index[name]; ok {
		c.settings[i] = Setting{Name: name, Value: value, File: file, Site: site}
		return
	}
	c.index[name] = len(c.settings)
	c.settings = append(c.settings, Setting{Name: name, Value: value, File: file, Site: site})
}

// Settings is the effective configuration.
func (c *Config) Settings() []Setting { return c.settings }

// Get returns a setting's effective value.
func (c *Config) Get(name string) (string, bool) {
	if i, ok := c.index[name]; ok {
		return c.settings[i].Value, true
	}
	return "", false
}

/*
Set writes a setting into this switch's own configuration.

Only ever into SiteDir, never into the image's copy: the image is replaced
wholesale by an upgrade, so a change written there would be lost by the next
one and would not show up for anyone comparing two switches.

The file is rewritten whole rather than appended to, so setting a key twice
leaves one line rather than two. Written to a temporary and renamed, because a
switch that loses power mid-write should come back with the old configuration
rather than half of the new one.

A nil value removes the setting, and the image's default applies again.
*/
func Set(name string, value *string) error {
	if !RAMBooted() {
		return SetIn(SiteDir, name, value)
	}

	// A RAM boot keeps its configuration in the bootloader's flash, and the
	// initramfs copies it into /mnt/data/config at the next boot. Write both:
	// flash so it survives, and the tmpfs so `config show` reflects it now
	// rather than only after a reboot.
	dir, unmount, err := mountFlashConfig()
	if err != nil {
		return fmt.Errorf("this board has no persistent data partition and its "+
			"flash could not be reached, so the setting would be lost at the "+
			"next reboot: %w", err)
	}
	defer unmount()

	if err := SetIn(dir, name, value); err != nil {
		return err
	}
	// Best effort: failing to mirror into the tmpfs costs a stale `config
	// show` until the next boot, not the setting itself.
	_ = SetIn(SiteDir, name, value)
	return nil
}

// mountFlashConfig mounts the bootloader's filesystem read-write and returns
// the configuration directory inside it.
//
// Read-write on a partition that also holds the boot images, so it is mounted
// for as long as one file takes to write and no longer, and only ever written
// inside its own subdirectory.
func mountFlashConfig() (string, func(), error) {
	mnt, err := os.MkdirTemp("", "nosaic-flash")
	if err != nil {
		return "", nil, err
	}
	for _, dev := range flashCandidates {
		if _, err := os.Stat(dev); err != nil {
			continue
		}
		cmd := exec.Command("mount", "-t", "vfat", dev, mnt)
		if out, err := cmd.CombinedOutput(); err != nil {
			_ = out
			continue
		}
		dir := filepath.Join(mnt, flashConfigDir)
		if err := os.MkdirAll(dir, 0o755); err != nil {
			_ = exec.Command("umount", mnt).Run()
			continue
		}
		return dir, func() {
			_ = exec.Command("sync").Run()
			_ = exec.Command("umount", mnt).Run()
			_ = os.Remove(mnt)
		}, nil
	}
	os.Remove(mnt)
	return "", nil, fmt.Errorf("no bootloader filesystem found on %s",
		strings.Join(flashCandidates, ", "))
}

// SetIn is Set with the directory named, for tests.
func SetIn(dir, name string, value *string) error {
	if name == "" || strings.ContainsAny(name, "=\n") {
		return fmt.Errorf("a setting name cannot be empty or contain '=' or a newline")
	}
	if value != nil && strings.ContainsRune(*value, '\n') {
		return fmt.Errorf("a setting value cannot contain a newline")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	path := filepath.Join(dir, SiteFile)

	var kept []string
	if b, err := os.ReadFile(path); err == nil {
		for _, line := range strings.Split(string(b), "\n") {
			t := strings.TrimSpace(line)
			if t == "" || strings.HasPrefix(t, "#") {
				continue
			}
			if k, _, ok := strings.Cut(t, "="); ok && strings.TrimSpace(k) == name {
				continue // replaced below, or removed
			}
			kept = append(kept, line)
		}
	} else if !os.IsNotExist(err) {
		return err
	}

	var b strings.Builder
	fmt.Fprintf(&b, "# This switch's own configuration. Written by `nosaic config set`.\n")
	fmt.Fprintf(&b, "# Overrides the image's defaults in %s.\n", ImageDir)
	for _, l := range kept {
		b.WriteString(l)
		b.WriteByte('\n')
	}
	if value != nil {
		fmt.Fprintf(&b, "%s=%s\n", name, *value)
	}

	tmp := path + ".new"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	if _, err := f.WriteString(b.String()); err != nil {
		f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}
