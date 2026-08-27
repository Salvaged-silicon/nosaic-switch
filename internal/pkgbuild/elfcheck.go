package pkgbuild

import (
	"debug/elf"
	"fmt"

	"github.com/salvaged-silicon/nosaic-switch/internal/arch"
	"github.com/salvaged-silicon/nosaic-switch/internal/nospkg"
)

// verifyELF checks that every ELF object in a package was built for the target.
//
// A cross-build that silently picks up the host compiler produces a package
// that builds cleanly, installs cleanly, and contains x86 objects for a
// PowerPC switch. Nothing notices until the switch does. Since the header
// carries machine, word size and endianness, checking is nearly free — and it
// is the difference between "we set CC correctly" and "we verified the output".
func verifyELF(a *arch.Arch, entries []nospkg.Entry) error {
	if a.ELFMachine == "" {
		return nil
	}
	want, ok := elfMachines[a.ELFMachine]
	if !ok {
		return fmt.Errorf("arch/%s: unknown elf_machine %q", a.ID, a.ELFMachine)
	}
	wantClass := elf.ELFCLASS64
	if a.Bits == 32 {
		wantClass = elf.ELFCLASS32
	}
	wantData := elf.ELFDATA2LSB
	if a.Endian == "big" {
		wantData = elf.ELFDATA2MSB
	}

	checked := 0
	for _, e := range entries {
		if e.Src == "" || e.Dir || e.Link != "" {
			continue
		}
		f, err := elf.Open(e.Src)
		if err != nil {
			continue // not an ELF: a header, a man page, a pkg-config file
		}
		machine, class, data := f.Machine, f.Class, f.Data
		f.Close()
		checked++

		if machine != want {
			return fmt.Errorf("%s is for %v, but this package targets %v — "+
				"the build used the wrong compiler", e.Dst, machine, want)
		}
		if class != wantClass {
			return fmt.Errorf("%s is %v, expected %v", e.Dst, class, wantClass)
		}
		if data != wantData {
			return fmt.Errorf("%s is %v, expected %v — wrong endianness", e.Dst, data, wantData)
		}
	}
	return nil
}

// elfMachines maps the name in arch.yml to the ELF header value. Named rather
// than numeric so an arch definition reads as documentation.
var elfMachines = map[string]elf.Machine{
	"EM_X86_64":  elf.EM_X86_64,
	"EM_386":     elf.EM_386,
	"EM_AARCH64": elf.EM_AARCH64,
	"EM_ARM":     elf.EM_ARM,
	"EM_PPC":     elf.EM_PPC,
	"EM_PPC64":   elf.EM_PPC64,
	"EM_MIPS":    elf.EM_MIPS,
}
