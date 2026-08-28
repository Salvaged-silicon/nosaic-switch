// Package board parses and validates board ports.
//
// A board port is one self-contained directory under platform/. Nothing
// central lists the supported boards: the catalog is derived by scanning
// these directories, so adding a switch means adding a folder and touching
// no shared file.
package board

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"gopkg.in/yaml.v3"

	"github.com/salvaged-silicon/nosaic-switch/internal/boot"
)

// Status is how far a port has got. Only "production" boards are advertised
// in the README — the project claims support for what works, not what is
// being attempted.
var validStatus = []string{"planned", "bringup", "experimental", "production"}

// validProfile matches the tiers in base/. See docs/DESIGN.md.
var validProfile = []string{"full", "slim", "minimal"}

// Board is one physical switch or router.
type Board struct {
	ID     string `yaml:"id"`
	Vendor string `yaml:"vendor"`
	Model  string `yaml:"model"`

	// The orthogonal axes. Each names a directory: arch/<arch>,
	// boot/<boot>. asic selects the nosd provider.
	Arch string `yaml:"arch"`
	ASIC string `yaml:"asic"`
	Boot string `yaml:"boot"`

	Profile string `yaml:"profile"`
	Kernel  string `yaml:"kernel"`
	Status  string `yaml:"status"`

	// U-Boot boards must state where in RAM the kernel is loaded. There is no
	// sensible default: an address that works on one SoC lands on top of
	// something important on another, and the symptom is a board that hangs
	// with nothing on the console.
	UBootArch  string `yaml:"u_boot_arch"`
	UBootLoad  string `yaml:"u_boot_load"`
	UBootEntry string `yaml:"u_boot_entry"`

	// Flash layout, in MiB. Zero means the default, which suits a board with
	// modest flash; a board with room should say so rather than inherit a
	// number chosen for a virtual machine.
	//
	// These were constants until it became clear what that meant: 96 MiB
	// slots on a switch with 2.1 GB free, where the vendor's own OS occupies
	// 347 MB and its successor 600 MB. An image that does not fit is then a
	// property of our arithmetic rather than of the hardware.
	BootMiB int `yaml:"boot_mib"`
	SlotMiB int `yaml:"slot_mib"`
	DataMiB int `yaml:"data_mib"`

	// KernelParams are appended to the kernel command line by the board's
	// installer. Board data because they describe this box's memory map.
	KernelParams string `yaml:"kernel_params"`

	// AbootMaxHWEpoch is board data for Arista boards; read it off the switch
	// with prefdl. Defaults to 1, which covers the 7050SX2.
	AbootMaxHWEpoch string `yaml:"aboot_max_hwepoch"`

	Notes string `yaml:"notes"`

	Path string `yaml:"-"`
}

// Load reads a single board.yml.
func Load(path string) (*Board, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var brd Board
	dec := yaml.NewDecoder(strings.NewReader(string(b)))
	dec.KnownFields(true)
	if err := dec.Decode(&brd); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	brd.Path = path
	return &brd, nil
}

// LoadAll scans platform/ for board ports. TEMPLATE is skipped: it is the
// scaffold contributors copy, not a board.
func LoadAll(root string) ([]*Board, error) {
	dir := filepath.Join(root, "platform")
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []*Board
	for _, e := range entries {
		if !e.IsDir() || e.Name() == "TEMPLATE" {
			continue
		}
		p := filepath.Join(dir, e.Name(), "board.yml")
		if _, err := os.Stat(p); os.IsNotExist(err) {
			continue
		}
		brd, err := Load(p)
		if err != nil {
			return nil, err
		}
		out = append(out, brd)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out, nil
}

// Validate returns every problem with this board port.
// Layout returns the flash layout in MiB, with defaults for anything the board
// does not state.
func (b *Board) Layout() (boot, slot, data int) {
	boot, slot, data = b.BootMiB, b.SlotMiB, b.DataMiB
	if boot == 0 {
		boot = 32
	}
	if slot == 0 {
		slot = 96
	}
	if data == 0 {
		data = 256
	}
	return
}

func (b *Board) Validate(root string) []string {
	var errs []string
	bad := func(f string, a ...any) { errs = append(errs, fmt.Sprintf(f, a...)) }

	if b.ID == "" {
		bad("id is required")
	} else if dir := filepath.Base(filepath.Dir(b.Path)); dir != b.ID {
		bad("id %q does not match its directory %q", b.ID, dir)
	}
	if b.Arch == "" {
		bad("arch is required")
	}
	if b.ASIC == "" {
		bad("asic is required")
	}
	if b.Boot == "" {
		bad("boot is required")
	}
	if !oneOf(b.Status, validStatus) {
		bad("status %q must be one of %s", b.Status, strings.Join(validStatus, ", "))
	}
	if !oneOf(b.Profile, validProfile) {
		bad("profile %q must be one of %s", b.Profile, strings.Join(validProfile, ", "))
	}

	// A slot smaller than the boot partition is almost certainly a
	// transposition. Nothing else about the proportions is checkable: the
	// first board to state a layout has 768 MiB slots and a 512 MiB data
	// partition, which an earlier version of this check called transposed
	// because it assumed data must exceed a slot. It need not -- the image is
	// large and the configuration it persists is small.
	if boot, slot, data := b.Layout(); slot < boot {
		bad("flash layout looks transposed: boot %d MiB is larger than a %d MiB slot (data %d MiB)",
			boot, slot, data)
	}

	// Checked here rather than at build time: a U-Boot board with no load
	// address cannot produce a bootable image, and finding that out after a
	// full build wastes an hour.
	if b.Boot == "uboot" {
		if b.UBootArch == "" {
			bad("boot is uboot, so u_boot_arch is required (ppc, arm, arm64, x86)")
		}
		if b.UBootLoad == "" || b.UBootEntry == "" {
			bad("boot is uboot, so u_boot_load and u_boot_entry are required: " +
				"they depend on where this board's RAM is and have no safe default")
		}
	}

	// The axes are directories. A board naming one that does not exist is a
	// port that cannot build, and saying so here is cheaper than finding out
	// during an image build.
	if b.Arch != "" {
		if _, err := os.Stat(filepath.Join(root, "arch", b.Arch)); os.IsNotExist(err) {
			bad("arch %q has no arch/%s directory", b.Arch, b.Arch)
		}
	}
	// Ask the registry, not the filesystem. What makes a bootloader supported
	// is a backend that can wrap an image for it; boot/<id>/ holds notes and
	// helper scripts and several backends need neither. Checking for the
	// directory would have rejected every board using aboot, onie-sfx or
	// uboot, none of which have one.
	if b.Boot != "" {
		if _, err := boot.For(b.Boot); err != nil {
			bad("boot %q is not a supported bootloader; have %s",
				b.Boot, strings.Join(boot.All(), ", "))
		}
	}

	return errs
}

func oneOf(v string, valid []string) bool {
	for _, s := range valid {
		if v == s {
			return true
		}
	}
	return false
}
