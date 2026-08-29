package nospkg

import (
	"fmt"
	"syscall"
)

// chmodRaw sets a file's permission bits from a Unix mode.
//
// It exists because os.Chmod does not do this. Go's os.FileMode is its own
// encoding -- setuid is 1<<23, setgid 1<<22, sticky 1<<20 -- so handing it a
// Unix 0o4755 or 0o1777 loses exactly the bit that was the point of saying so.
// syscall.Chmod takes the raw mode, which is what a tar header carries.
func chmodRaw(path string, mode int64) error {
	// Only the permission and set-id/sticky bits; a tar header's mode field
	// can carry type bits that have no business being applied here.
	m := uint32(mode) & 0o7777
	if m == 0 {
		// A mode of zero is not a request for an unreadable file; it means the
		// archive did not say. Leaving it alone is better than making the file
		// unusable.
		return nil
	}
	if err := syscall.Chmod(path, m); err != nil {
		return fmt.Errorf("setting mode %04o on %s: %w", m, path, err)
	}
	return nil
}
