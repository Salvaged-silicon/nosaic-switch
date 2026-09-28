package main

import (
	"encoding/binary"
	"fmt"
	"testing"
)

// /proc/net/route prints the address's in-memory word as hex, in the host's
// byte order. The C CLI once read it one way everywhere and reversed every
// route on the PowerPC switch; this builds the kernel's text the way the
// kernel would on this host and checks it reads back as the address.
func TestProcHexIsHostOrder(t *testing.T) {
	for _, a := range [][4]byte{{192, 168, 0, 0}, {10, 101, 255, 53}, {255, 255, 255, 248}} {
		text := fmt.Sprintf("%08X", binary.NativeEndian.Uint32(a[:]))
		if got, want := procHex(text), binary.BigEndian.Uint32(a[:]); got != want {
			t.Errorf("%v printed as %s reads back as %08x, want %08x", a, text, got, want)
		}
	}
}
