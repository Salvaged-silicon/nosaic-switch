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

// Status is how far a port has got, and it is stated rather than filtered on.
//
// The README lists every board with its status beside it. Hiding the ones that
// are not finished sounds honest and is not: somebody with an AS5610 in a rack
// wants to know it boots, forwards and is short of a few things, and a table
// that omits it tells them the project has never heard of their switch.
//
// Nothing is described as "production" until it has run somewhere that matters
// for longer than a lab afternoon.
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

	// UBootStage is where a TFTP'd image is parked before bootm unpacks it.
	// Must not overlap where the kernel unpacks, which is UBootLoad.
	UBootStage string `yaml:"u_boot_stage"`

	// Where the device tree and initramfs are placed. Empty lets U-Boot
	// choose. An older U-Boot may need to be told.
	// The digest inside the FIT. Empty means sha256; an older U-Boot may know
	// only crc32.
	UBootFITHash string `yaml:"u_boot_fit_hash"`

	// UBootNOSBootCmd is what the installer writes into the firmware's
	// nos_bootcmd, telling it where the NOS now lives.
	//
	// Board data because the commands differ per firmware: this board reaches
	// its disk with `usb start; usbiddev` and loads a raw partition with
	// usbboot, where another U-Boot would use ide, scsi or mmc. A generated
	// guess would be wrong on every board but the one it was written for.
	UBootNOSBootCmd string `yaml:"u_boot_nos_bootcmd"`

	UBootFDTAddr     string `yaml:"u_boot_fdt_addr"`
	UBootRamdiskAddr string `yaml:"u_boot_ramdisk_addr"`

	// NetWaitSecs bounds how long the network service waits for interfaces
	// named in network.conf to appear. Zero means the default.
	//
	// Front-panel interfaces exist only once the datapath daemon has created
	// them, so waiting is right -- but a board whose datapath does not exist
	// yet waits the whole time for interfaces that cannot appear, and the
	// service transition it is part of gives up first.
	NetWaitSecs int `yaml:"net_wait_secs"`

	// DeviceTree is a .dts under the board directory, compiled at image time
	// and carried inside the FIT.
	//
	// x86 boards describe themselves through ACPI and leave this empty. A
	// PowerPC or ARM board cannot boot without one, and a board's device tree
	// is board data in the same sense its port map is -- it belongs in the
	// board directory, not in the kernel recipe, so that adding a board stays
	// a matter of adding a directory.
	DeviceTree string `yaml:"device_tree"`

	// FrontPanelInit is a script under the board directory that brings the
	// optics up: transmitters, cage control lines, retimers.
	//
	// Boards with a platform HAL driver do this through `nosaic platform tx`,
	// which is where it belongs. A board being brought up does not have one
	// yet, and the alternative to naming a script here is that its ports stay
	// dark until it does -- which makes the datapath untestable for as long as
	// the HAL takes. The two are not exclusive: a board grows a HAL and drops
	// this field, and nothing above either notices.
	//
	// Installed to /etc/nosaic and run once, before nosd, since the daemon
	// reads link state during bring-up and would see every port down.
	FrontPanelInit string `yaml:"front_panel_init"`

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

	// PartitionTable is "gpt" (the default) or "dos".
	//
	// Not a style preference. A bootloader that cannot read the table cannot
	// find anything on the disk, and the failure is total: U-Boot 2013.01 on
	// the AS5610 has no GPT support compiled in at all -- its binary contains
	// no EFI or GUID partition strings, only "## Unknown partition table" --
	// so a GPT install there produces a switch that boots nothing. Stated by
	// the board because it is a property of that board's firmware.
	PartitionTable string `yaml:"partition_table"`

	// FITMiB is a raw partition holding the FIT image U-Boot loads, for
	// boards whose firmware reads a partition rather than a filesystem.
	//
	// Zero means the board does not need one. Where it is set, the installer
	// writes the FIT to that partition verbatim and points the firmware's own
	// boot command at it -- which is the mechanism this board's vendor OS
	// already uses, and therefore the one mechanism known to work here.
	FITMiB int `yaml:"fit_mib"`

	// KernelParams are appended to the kernel command line by the board's
	// installer. Board data because they describe this box's memory map.
	KernelParams string `yaml:"kernel_params"`

	// Console is the serial device and speed a login is offered on. Board data
	// because it is a property of the box: this fleet's Aristas run their
	// consoles at 9600, and a getty at any other speed reconfigures the port
	// and makes everything after it unreadable. Empty means ttyS0 at 115200,
	// which suits a VM.
	Console     string `yaml:"console"`
	ConsoleBaud int    `yaml:"console_baud"`

	// AbootMaxHWEpoch is board data for Arista boards; read it off the switch
	// with prefdl. Defaults to 1, which covers the 7050SX2.
	AbootMaxHWEpoch string `yaml:"aboot_max_hwepoch"`

	// PlatformHAL names the driver for the board's own hardware -- resets,
	// watchdog, sensors -- and where it lives. It is board data rather than
	// something the driver guesses because two switches with the same silicon
	// can have entirely different controllers, and because a PCI address hunted
	// for at runtime is a PCI address that can be found wrong.
	PlatformHAL PlatformHAL `yaml:"platform_hal"`

	// Thermal is where this board wants its fans. Board data because it
	// varies with airflow and silicon: a chassis that wants full cooling at
	// 65 °C and one that wants it at 55 °C are the same code and different
	// numbers.
	Thermal Thermal `yaml:"thermal"`

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
// ConsolePort returns the console device and speed, with defaults.
func (b *Board) ConsolePort() (dev string, baud int) {
	dev, baud = b.Console, b.ConsoleBaud
	if dev == "" {
		dev = "ttyS0"
	}
	if baud == 0 {
		baud = 115200
	}
	return
}

// Layout returns the flash layout in MiB, with defaults for anything the board
// does not state.
// PartTable is the partition table type this board's firmware can read.
func (b *Board) PartTable() string {
	if b.PartitionTable == "" {
		return "gpt"
	}
	return b.PartitionTable
}

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
		// The kernel spells its console "ttyS0,115200" and getty is handed the
		// device alone, so this field is the device alone. Getting it wrong costs
		// a board that boots correctly all the way to a getty that cannot open
		// what it was given, and shows nothing.
		if strings.Contains(b.Console, ",") {
			bad("console is the device on its own (%q), with the speed in console_baud: "+
				"getty is given the device, not the kernel's console= string",
				b.Console)
		}

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

// PlatformHAL is a board's platform-hardware driver and its addresses.
type PlatformHAL struct {
	// Driver selects the implementation; empty means the board has none yet.
	Driver string `yaml:"driver"`
	// PCI is where the controller is, in full domain:bus:dev.fn form.
	PCI string `yaml:"pci"`
	// ASICPCI is where the switch chip appears once released from reset. It
	// is absent from the bus until then, which is why it is stated here
	// rather than discovered.
	ASICPCI string `yaml:"asic_pci"`
}

// Thermal is a board's cooling curve.
type Thermal struct {
	// MinC and below sits at the fan floor; MaxC and above is flat out.
	MinC int `yaml:"min_c"`
	MaxC int `yaml:"max_c"`
	// SlewDownPercent is how much the duty may fall in one interval. Rises
	// are immediate; falling fast makes fans oscillate around a threshold.
	SlewDownPercent int `yaml:"slew_down_percent"`
	// IntervalSeconds between samples.
	IntervalSeconds int `yaml:"interval_seconds"`
}

// DatapathPackage is the package providing this board's `nosd`.
//
// Derived from the ASIC rather than stated, so one package serves every switch
// with a given chip and a board port does not have to name it. A board with no
// forwarding silicon returns the empty string.
func (b *Board) DatapathPackage() string {
	if b.ASIC == "" || b.ASIC == "none" {
		return ""
	}
	return "nosd-" + b.ASIC
}
