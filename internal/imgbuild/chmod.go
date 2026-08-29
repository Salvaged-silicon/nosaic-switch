package imgbuild

import (
	"fmt"
	"syscall"
)

// chmodRaw sets permission bits from a Unix mode, including setuid, setgid and
// sticky.
//
// os.Chmod cannot do this. Its os.FileMode is Go's own encoding -- setuid is
// 1<<23, setgid 1<<22, sticky 1<<20 -- so handing it a Unix 0o4755 or 0o1777
// silently drops exactly the bit that was the reason for saying so. The same
// mistake exists in internal/nospkg for the same reason.
func chmodRaw(path string, mode uint32) error {
	m := mode & 0o7777
	if err := syscall.Chmod(path, m); err != nil {
		return fmt.Errorf("setting mode %04o on %s: %w", m, path, err)
	}
	return nil
}
