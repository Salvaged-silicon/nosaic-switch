package imgbuild

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// usrMerge moves /bin, /sbin and /lib into /usr and leaves symlinks behind.
//
// This is not tidiness. systemd removed support for a split /usr in v255, and
// we build 257: it resolves helpers by absolute path under /usr and its default
// PATH does not include /bin or /sbin. On a split tree the symptoms are
// scattered and none of them names the cause -- "Failed to mount Temporary
// Directory /tmp" because it cannot find mount, and a shell script dying with
// "grep: not found" while /bin/grep exists.
//
// Applied to every profile rather than only the systemd ones. One layout is
// easier to reason about than two, the busybox and s6 tiers do not care which
// they get, and a difference that exists only on the tier nobody tests is the
// kind that is discovered on hardware.
func usrMerge(rootfs string, log io.Writer) error {
	for _, dir := range []string{"bin", "sbin", "lib", "lib64"} {
		src := filepath.Join(rootfs, dir)
		fi, err := os.Lstat(src)
		if os.IsNotExist(err) {
			continue
		}
		if err != nil {
			return err
		}
		// Already a symlink: either we have run, or the package tree shipped
		// it merged. Either way there is nothing to move.
		if fi.Mode()&os.ModeSymlink != 0 {
			continue
		}

		dst := filepath.Join(rootfs, "usr", dir)
		if err := os.MkdirAll(dst, 0o755); err != nil {
			return err
		}
		if err := mergeDir(src, dst, rootfs, log); err != nil {
			return fmt.Errorf("usr-merge %s: %w", dir, err)
		}
		if err := os.RemoveAll(src); err != nil {
			return err
		}
		if err := os.Symlink("usr/"+dir, src); err != nil {
			return err
		}
	}
	return nil
}

// mergeDir moves everything in src into dst.
//
// When a name exists in both, the copy already under /usr wins and the other
// is dropped, with a line saying so. The choice is not arbitrary: a package
// that installs to /usr/sbin has been merged upstream and knows the layout,
// and the case this actually arises for is halt, reboot, poweroff and
// shutdown, where busybox ships its own and systemd ships links to systemctl.
// On a system running systemd those must be systemd's, or asking the box to
// shut down does not reach PID 1.
//
// It is reported rather than silent because a dropped binary is exactly the
// kind of thing that is invisible until the one moment it is needed.
func mergeDir(src, dst, rootfs string, log io.Writer) error {
	entries, err := os.ReadDir(src)
	if err != nil {
		return err
	}
	for _, e := range entries {
		from := filepath.Join(src, e.Name())
		to := filepath.Join(dst, e.Name())

		if _, err := os.Lstat(to); err == nil {
			// Directories can be combined; anything else is a conflict.
			fromDir := e.IsDir()
			toInfo, _ := os.Lstat(to)
			if fromDir && toInfo.IsDir() {
				if err := mergeDir(from, to, rootfs, log); err != nil {
					return err
				}
				continue
			}
			if fromDir != toInfo.IsDir() {
				return fmt.Errorf("%s is a directory on one side and not the other", e.Name())
			}
			rel, _ := filepath.Rel(rootfs, from)
			keep, _ := filepath.Rel(rootfs, to)
			fmt.Fprintf(log, "    usr-merge: keeping /%s, dropping /%s\n", keep, rel)
			if err := os.RemoveAll(from); err != nil {
				return err
			}
			continue
		}
		if err := os.Rename(from, to); err != nil {
			return err
		}
	}
	return nil
}
