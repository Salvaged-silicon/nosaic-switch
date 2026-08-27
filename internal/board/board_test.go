package board

import (
	"path/filepath"
	"strings"
	"testing"
)

// Validate resolves arch/ against the repo root, and `go test` runs in the
// package directory.
const repoRoot = "../.."

func ubootBoard() *Board {
	return &Board{
		ID: "b", Path: filepath.Join("platform", "b", "board.yml"),
		Arch: "powerpc", ASIC: "none", Boot: "uboot",
		Status: "bringup", Profile: "minimal",
	}
}

// A U-Boot board with no load address cannot produce a bootable image. Caught
// here rather than at build time, because finding it out after a full build
// wastes an hour -- and finding it out on the console wastes an afternoon.
func TestUBootBoardMustStateItsAddresses(t *testing.T) {
	for _, tc := range []struct {
		name string
		fix  func(*Board)
		want string
	}{
		{"no arch at all", func(b *Board) {}, "u_boot_arch"},
		{"arch but no addresses", func(b *Board) { b.UBootArch = "ppc" }, "u_boot_load"},
		{"load but no entry", func(b *Board) {
			b.UBootArch, b.UBootLoad = "ppc", "0x1000000"
		}, "u_boot_entry"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			b := ubootBoard()
			tc.fix(b)
			errs := strings.Join(b.Validate(repoRoot), "; ")
			if !strings.Contains(errs, tc.want) {
				t.Errorf("wanted a complaint about %s, got %q", tc.want, errs)
			}
		})
	}
}

func TestCompleteUBootBoardValidates(t *testing.T) {
	b := ubootBoard()
	b.UBootArch, b.UBootLoad, b.UBootEntry = "ppc", "0x1000000", "0x1000000"
	if errs := b.Validate(repoRoot); len(errs) != 0 {
		t.Errorf("a complete uboot board was rejected: %q", errs)
	}
}

// The addresses are meaningless on the other bootloaders, so requiring them
// there would be a needless obstacle to every non-U-Boot port.
func TestOtherBootloadersDoNotNeedAddresses(t *testing.T) {
	b := ubootBoard()
	b.Boot = "onie-sfx"
	for _, e := range b.Validate(repoRoot) {
		if strings.Contains(e, "u_boot") {
			t.Errorf("an ONIE board was asked for a U-Boot address: %s", e)
		}
	}
}
