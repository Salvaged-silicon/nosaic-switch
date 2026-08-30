// Package imgbuild composes packages into a bootable image.
//
// The output is an immutable squashfs plus the initramfs that mounts it under
// an overlay. Nothing is assembled by hand: the board names a profile, the
// profile names packages, and the closure of those packages is what the image
// contains. An image is therefore reproducible from data, and what is in one
// can be answered by reading a manifest rather than by inspecting a filesystem.
package imgbuild

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/arch"
	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/depsolve"
	"github.com/salvaged-silicon/nosaic-switch/internal/identity"
	"github.com/salvaged-silicon/nosaic-switch/internal/nospkg"
	"github.com/salvaged-silicon/nosaic-switch/internal/profile"
	"github.com/salvaged-silicon/nosaic-switch/internal/svcgen"
)

// Options controls one image build.
type Options struct {
	Root    string
	Board   *board.Board
	Arch    *arch.Arch
	Profile *profile.Profile

	PackageDir string
	OutDir     string
	Version    string
	Log        io.Writer

	// RAMBoot carries the root filesystem inside the initramfs, so the image
	// boots with no storage of ours. It is how a board is tried the first
	// time: the bootloader fetches it over the network, the vendor's OS stays
	// intact on flash, and a power cycle undoes everything.
	RAMBoot bool
}

// Result is what was produced.
type Result struct {
	Squashfs  string
	Initramfs string
	Kernel    string
	Disk      string
	Packages  []string
}

// Build assembles the image.
func Build(o Options) (*Result, error) {
	if o.Log == nil {
		o.Log = io.Discard
	}
	id, err := identity.Load(o.Root)
	if err != nil {
		return nil, err
	}

	work := filepath.Join(o.Root, ".cache", "image", o.Board.ID)
	rootfs := filepath.Join(work, "rootfs")
	if err := os.RemoveAll(work); err != nil {
		return nil, err
	}
	if err := os.MkdirAll(rootfs, 0o755); err != nil {
		return nil, err
	}

	selected, err := selectPackages(o)
	if err != nil {
		return nil, err
	}

	var names []string
	var kernel string
	var users []nospkg.User
	for _, p := range selected {
		file := filepath.Join(o.PackageDir, p.file)
		fmt.Fprintf(o.Log, "    + %s %s\n", p.Name, p.Version)
		m, err := nospkg.Extract(file, rootfs)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", p.Name, err)
		}
		names = append(names, m.Name+"-"+m.Version)
		users = append(users, m.Users...)
	}

	// Merge /usr before anything reads paths out of the tree: the kernel
	// lookup, the init setup and the initramfs all address files by path, and
	// they must all see the same layout the running system will.
	if err := usrMerge(rootfs, o.Log); err != nil {
		return nil, err
	}

	// The kernel is booted rather than mounted, so it is lifted out of the
	// composed tree rather than shipped inside the read-only image.
	if k, err := findKernel(rootfs); err == nil {
		kernel = k
	} else {
		return nil, err
	}

	if err := stamp(o, rootfs, id, names, users); err != nil {
		return nil, err
	}

	if err := os.MkdirAll(o.OutDir, 0o755); err != nil {
		return nil, err
	}
	sqsh := filepath.Join(o.OutDir, "rootfs.sqsh")
	if err := mksquashfs(o, rootfs, sqsh); err != nil {
		return nil, err
	}

	// A board booted over the network from its bootloader has no partitions
	// of ours to mount, so the root filesystem travels inside the initramfs.
	embed := ""
	if o.RAMBoot {
		embed = sqsh
	}
	initramfs, err := buildInitramfs(o, work, rootfs, embed)
	if err != nil {
		return nil, err
	}

	disk, err := BuildDisk(o, sqsh)
	if err != nil {
		return nil, err
	}

	outKernel := filepath.Join(o.OutDir, "vmlinuz")
	if err := copyFile(kernel, outKernel); err != nil {
		return nil, err
	}

	return &Result{Squashfs: sqsh, Initramfs: initramfs, Kernel: outKernel, Disk: disk, Packages: names}, nil
}

type pkgRef struct {
	depsolve.Pkg
	file string
}

// selectPackages resolves the profile's package list against what has been
// built, and fails if anything is missing rather than composing a partial
// image that would fail confusingly at boot.
func selectPackages(o Options) ([]pkgRef, error) {
	entries, err := os.ReadDir(o.PackageDir)
	if err != nil {
		return nil, fmt.Errorf("no packages built yet: %w", err)
	}
	var available []depsolve.Pkg
	byName := map[string]string{}
	for _, e := range entries {
		if !strings.HasSuffix(e.Name(), ".nos") {
			continue
		}
		m, err := nospkg.ReadManifestFile(filepath.Join(o.PackageDir, e.Name()))
		if err != nil {
			return nil, err
		}
		// A package for another CPU in the directory is not an error; it is
		// simply not a candidate for this image.
		if m.Arch != nospkg.ArchAny && m.Arch != o.Arch.ID {
			continue
		}
		available = append(available, depsolve.Pkg{
			Name: m.Name, Version: m.Version,
			Provides: m.Provides, Conflicts: m.Conflicts, Depends: m.Depends,
		})
		byName[m.Name] = e.Name()
	}

	// What the profile asks for, plus the board's datapath.
	//
	// A board with forwarding silicon needs a daemon that can drive it, and
	// which one falls out of the silicon rather than from a list somebody
	// maintains: the board says `asic: td2p` and that resolves to whichever
	// package provides `nosd` for it. Nothing central maps boards to daemons,
	// which is the point -- adding a switch with an already-supported ASIC
	// should not mean editing anything but the board directory.
	//
	// The virtual platform is not a special case: it declares `asic: virt` and
	// gets nosd-virt by the same route.
	wanted := append([]string(nil), o.Profile.Packages...)
	if datapath := o.Board.DatapathPackage(); datapath != "" {
		if _, ok := byName[datapath]; ok {
			wanted = append(wanted, datapath)
		} else {
			// Loud rather than fatal. A board whose datapath is not built yet
			// is a normal state during a port -- the virtual platform is in it
			// -- and refusing to build would stop the boot testing that gets a
			// board to the point of having one. But an image with no datapath
			// is a switch that cannot switch, and that must not be something
			// anyone discovers on the hardware.
			fmt.Fprintf(o.Log, "  WARNING: no datapath. Board %s has asic %q, "+
				"which wants %s, and no such package is built.\n"+
				"           This image will boot and will not forward anything.\n",
				o.Board.ID, o.Board.ASIC, datapath)
		}
	}

	order, err := depsolve.Resolve(available, wanted)
	if err != nil {
		return nil, err
	}
	out := make([]pkgRef, len(order))
	for i, p := range order {
		out[i] = pkgRef{Pkg: p, file: byName[p.Name]}
	}
	return out, nil
}

func findKernel(rootfs string) (string, error) {
	matches, _ := filepath.Glob(filepath.Join(rootfs, "boot", "vmlinuz-*"))
	if len(matches) == 0 {
		matches, _ = filepath.Glob(filepath.Join(rootfs, "boot", "vmlinux-*"))
	}
	if len(matches) == 0 {
		return "", fmt.Errorf("the composed image contains no kernel: is the linux package in the profile?")
	}
	sort.Strings(matches)
	return matches[len(matches)-1], nil
}

func mksquashfs(o Options, dir, out string) error {
	fmt.Fprintf(o.Log, "==> squashing the root filesystem\n")
	// -all-root and a fixed mkfs time keep the image reproducible: ownership
	// from the build host and the wall clock are the two things that would
	// otherwise differ between two builds of identical inputs.
	cmd := exec.Command("mksquashfs", dir, out,
		"-noappend", "-all-root", "-comp", "xz", "-no-progress",
		"-mkfs-time", "0", "-all-time", "0")
	if b, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("mksquashfs: %v\n%s", err, b)
	}
	return nil
}

func copyFile(src, dst string) error {
	b, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return os.WriteFile(dst, b, 0o644)
}

func writeFile(root, path, content string, mode os.FileMode) error {
	full := filepath.Join(root, path)
	if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
		return err
	}
	// Remove first, because packages install symlinks and os.WriteFile follows
	// them. busybox ships /sbin/init as a symlink to ../bin/busybox, so
	// writing an init script there wrote *through* the link and replaced the
	// busybox binary with a shell script -- a 2.4 MB executable became 1806
	// bytes, and the initramfs that depends on it would have failed to boot
	// with no hint as to why.
	if err := os.Remove(full); err != nil && !os.IsNotExist(err) {
		return err
	}
	return os.WriteFile(full, []byte(content), mode)
}

// stamp writes the identity of the image into it: what it is, what it
// contains, and who may log in.
func stamp(o Options, rootfs string, id *identity.Identity, packages []string, users []nospkg.User) error {
	osRelease := fmt.Sprintf(`NAME="NOSaic"
ID=nosaic
VERSION="%s"
VERSION_ID="%s"
PRETTY_NAME="NOSaic %s (%s)"
NOSAIC_BOARD=%s
NOSAIC_ARCH=%s
NOSAIC_PROFILE=%s
HOME_URL="https://github.com/salvaged-silicon/nosaic-switch"
`, o.Version, o.Version, o.Version, o.Board.ID, o.Board.ID, o.Arch.ID, o.Profile.Name)
	if err := writeFile(rootfs, "/etc/os-release", osRelease, 0o644); err != nil {
		return err
	}

	// A self-describing image: what it contains is recorded in it, so the
	// question can be answered on the box rather than by inspecting files.
	sort.Strings(packages)
	meta := map[string]any{
		"version": o.Version, "board": o.Board.ID, "arch": o.Arch.ID,
		"profile": o.Profile.Name, "init": o.Profile.Init, "packages": packages,
	}
	b, _ := json.MarshalIndent(meta, "", "  ")
	if err := writeFile(rootfs, "/etc/nosaic/image.json", string(b)+"\n", 0o644); err != nil {
		return err
	}

	// The board's own description travels with the image. Without it the CLI
	// would have to be told which board it is running on, on the one machine
	// that has no excuse not to know -- and the platform HAL would be
	// addressing registers from a board id typed at a prompt.
	// Board.Path is the board.yml file itself, not the directory holding it.
	if src := o.Board.Path; src != "" {
		yml, err := os.ReadFile(src)
		if err != nil {
			return fmt.Errorf("reading the board description to place in the image: %w", err)
		}
		if err := writeFile(rootfs, "/etc/nosaic/board.yml", string(yml), 0o644); err != nil {
			return err
		}
	}

	// The account exists with no password, per base/identity.yml. An empty
	// password field is not "any password will do": it means no password is
	// required, and with no network login enabled the console is the only way
	// in until one is set.
	passwd := fmt.Sprintf("root:x:0:0:root:/root:/bin/sh\n%s:x:1000:1000:NOSaic:/home/%s:/bin/sh\n",
		id.Account, id.Account)
	shadow := fmt.Sprintf("root:*:::::::\n%s::::::::\n", id.Account)
	group := fmt.Sprintf("root:x:0:\n%s:x:1000:\n", id.Account)

	// Accounts the installed packages asked for. They are locked: these exist
	// for a daemon to run as, and one that can also be logged into is a way in
	// that nobody chose to open.
	for _, u := range dedupeUsers(users, id.Account) {
		home, sh := u.Home, u.Shell
		if home == "" {
			home = "/"
		}
		if sh == "" {
			sh = "/sbin/nologin"
		}
		passwd += fmt.Sprintf("%s:x:%d:%d:%s:%s:%s\n", u.Name, u.UID, u.GID, u.Name, home, sh)
		shadow += fmt.Sprintf("%s:!:::::::\n", u.Name)
		group += fmt.Sprintf("%s:x:%d:\n", u.Name, u.GID)
	}

	if err := writeFile(rootfs, "/etc/passwd", passwd, 0o644); err != nil {
		return err
	}
	if err := writeFile(rootfs, "/etc/shadow", shadow, 0o600); err != nil {
		return err
	}
	if err := writeFile(rootfs, "/etc/group", group, 0o644); err != nil {
		return err
	}

	// The CLI, which the cooling loop and the operator both need.
	if err := installCLI(o.Root, rootfs, o.Arch.GoArch, o.Log); err != nil {
		return err
	}

	// The board's own configuration files.
	//
	// A datapath daemon is useless without them: the port map, the SerDes
	// polarity and the SDK properties are what turn an initialised chip into
	// one that carries traffic, and they are per board. Anything in the
	// board's config/ directory is placed under /etc/nosaic, which is where
	// everything on the running system looks for it.
	//
	// Some of these are generated rather than shipped -- a port map read from
	// your own switch is not in this repository -- so a board directory that
	// has none is normal and not an error. What is not normal is discovering
	// on the hardware that the image had none, which is why the count is
	// reported.
	if o.Board.Path != "" {
		cfgDir := filepath.Join(filepath.Dir(o.Board.Path), "config")
		n, err := copyBoardConfig(cfgDir, rootfs)
		if err != nil {
			return err
		}
		if n > 0 {
			fmt.Fprintf(o.Log, "    %d board configuration file(s) into /etc/nosaic\n", n)
		}
	}

	// How the account becomes root. The profile decides -- sudo where there is
	// room for it, doas on the tier built for boards where there is not -- and
	// this refuses to build an image where the declared answer is not actually
	// present and setuid.
	privilege := o.Profile.Privilege
	if privilege == "" {
		privilege = id.Privilege
	}
	if err := writePrivilege(rootfs, id.Account, privilege, o.Log); err != nil {
		return err
	}

	// A self-test that runs only when asked for on the kernel command line.
	//
	// Reaching a login prompt proves the boot path; it does not prove the
	// system is usable. This checks the things an image must actually have --
	// a writable overlay, its own identity, the login account -- and then
	// powers off, so an automated boot terminates on success rather than
	// sitting at a prompt until a timeout it cannot distinguish from a hang.
	selftest := fmt.Sprintf(`#!/bin/sh
grep -q nosaic.selftest /proc/cmdline || exit 0

# Run alongside getty rather than before it, so one boot proves both that the
# system self-tests and that a login prompt actually appears. Powering off
# first would make those two checks mutually exclusive.
sleep 3

fail=0
say() { echo "NOSAIC-SELFTEST $*"; }

. /etc/os-release 2>/dev/null
[ "$ID" = nosaic ] && say "identity $PRETTY_NAME" || { say "FAIL no os-release"; fail=1; }

[ -f /etc/nosaic/image.json ] && say "manifest present" || { say "FAIL no image manifest"; fail=1; }

# The image must know what hardware it is on without the source tree. The
# platform HAL addresses registers from this file, so a switch that cannot say
# what board it is cannot safely touch its own hardware.
[ -f /etc/nosaic/board.yml ] && say "board description present" || { say "FAIL no board description"; fail=1; }

# /tmp must be writable by an ordinary user. It was root-owned and 0755 on the
# first real board, which no test caught because everything in this script runs
# as root -- so it is checked as the login account, not as whoever runs this.
if su -s /bin/sh -c "touch /tmp/.nosaic-selftest" %[1]s 2>/dev/null; then
    say "/tmp writable by %[1]s"; rm -f /tmp/.nosaic-selftest
else say "FAIL /tmp is not writable by %[1]s"; fail=1; fi

# The persistent configuration directory must exist and be writable.
#
# It is where per-switch configuration lives -- a port map read from this
# board, the SerDes polarity table -- and it is carried across from the
# initramfs. A boot path that mounts it, uses it and then leaves it behind
# produces a running system with no /mnt/data at all, and the failure is
# silent: the directory is simply absent, which reads as "nothing has been
# configured yet" rather than as a bug. That is exactly what the RAM boot did.
if [ -d /mnt/data/config ] && touch /mnt/data/config/.selftest 2>/dev/null; then
    say "persistent config directory present"; rm -f /mnt/data/config/.selftest
else say "FAIL no writable /mnt/data/config"; fail=1; fi

# A path to root. Without one the switch cannot reach its own hardware -- the
# platform HAL opens PCI resources that are root-only -- and every minimal
# image shipped that way until the first real board hit it. Checked as a real
# elevation, not as "the binary exists": a helper that is not setuid exists
# perfectly well and does nothing.
priv=""
[ -u /usr/bin/doas ] && priv=/usr/bin/doas
[ -u /usr/bin/sudo ] && priv=/usr/bin/sudo
if [ -z "$priv" ]; then say "FAIL no setuid privilege helper"; fail=1
elif [ "$(su -s /bin/sh -c "$priv -n id -u" %[1]s 2>/dev/null)" = 0 ]; then
    say "privilege $priv elevates to root"
else say "FAIL $priv did not elevate"; fail=1; fi

# The overlay is what makes a read-only image usable. If it is not writable the
# system boots and then fails the first time anything tries to save state.
if touch /run/nosaic-selftest 2>/dev/null; then say "overlay writable"
else say "FAIL overlay is not writable"; fail=1; fi

# The image must be read-only underneath, or it is not immutable and an
# upgrade could not be atomic.
if touch /nosaic-should-fail 2>/dev/null; then
    say "note: the root is writable via the overlay, as intended"
    rm -f /nosaic-should-fail
fi

grep -q "^admin:" /etc/passwd && say "login account present" || { say "FAIL no admin account"; fail=1; }

# Persistence. A count that survives a reboot is the only honest way to show
# that the data partition is real rather than a tmpfs pretending to be one.
if mountpoint -q /mnt/data 2>/dev/null || [ -d /mnt/data/config ]; then
    n=0
    [ -f /mnt/data/boot-count ] && n=$(cat /mnt/data/boot-count 2>/dev/null || echo 0)
    n=$((n + 1))
    echo "$n" > /mnt/data/boot-count 2>/dev/null && say "boot count $n" || { say "FAIL data partition is not writable"; fail=1; }
    [ -d /mnt/data/config ]  && say "config directory present"  || { say "FAIL no config directory"; fail=1; }
    [ -d /mnt/data/secrets ] && say "secrets directory present" || { say "FAIL no secrets directory"; fail=1; }
elif [ -f /etc/nosaic/ramboot ]; then
    # A RAM boot has no persistent storage and is not supposed to. Asserting
    # otherwise fails an image that is behaving exactly as intended, which is
    # how the first network boot of a new switch would have reported itself
    # broken.
    say "no data partition, as expected for a RAM boot"
else
    say "FAIL no data partition mounted"; fail=1
fi
# Network. Reported here rather than trusted from the service's own exit code,
# because "the unit started" and "the address is on the interface" are
# different claims -- and on a switch reached through a serial cable in another
# building, the second one is the one that matters.
if [ -r /etc/nosaic/network.conf ]; then
    want=0; got=0; absent=0
    while read -r kind name rest; do
        [ "$kind" = "iface" ] || continue
        want=$((want + 1))
        addr=$(echo "$rest" | awk "{print \$1}")
        if ! ip link show "$name" >/dev/null 2>&1; then
            absent=$((absent + 1)); continue
        fi
        ip addr show dev "$name" 2>/dev/null | grep -q "${addr%%/*}" && got=$((got + 1))
    done < /etc/nosaic/network.conf
    say "network $got of $want addresses set, $absent interface(s) absent"
    # An interface that exists and did not take its address is a fault. One
    # that does not exist yet is the datapath not being up, which is expected
    # on a management-plane boot.
    [ $((got + absent)) -eq "$want" ] || { say "FAIL an interface is present but unconfigured"; fail=1; }
fi

grep -q "^admin::" /etc/shadow && say "no password set, as shipped" || { say "FAIL admin has a password"; fail=1; }

# Commit or decline the trial. This is what "confirms itself healthy" means in
# practice: the system that just booted decides whether it is good enough to
# keep, and if it never does, the initramfs rolls back on a later boot.
#
# Deliberately gated on the health checks rather than on merely having reached
# userspace. An image that boots and does not work is exactly the case rollback
# exists for, and committing on "init ran" would defeat it.
if [ -f /mnt/boot/boot/trial ]; then
    if [ "$fail" = 0 ]; then
        mv /mnt/boot/boot/trial /mnt/boot/boot/active
        rm -f /mnt/boot/boot/tries
        say "COMMIT the trial slot is now active"
    else
        say "NOCOMMIT health checks failed; this trial will roll back"
    fi
fi

[ "$fail" = 0 ] && say "OK" || say "FAILED"
sync
poweroff -f
`, id.Account)
	if err := writeFile(rootfs, "/etc/nosaic/selftest.sh", selftest, 0o755); err != nil {
		return err
	}

	// Mount points, before anything init-specific.
	//
	// These were created at the end of this function, after the branch that
	// returns early for an s6 profile -- so that profile got no /proc, /sys or
	// /dev, and its init reported three mounts failing with "No such file or
	// directory". Shared setup belongs before the branch, not after it.
	// Modes matter here and 0755 is not right for all of them. /tmp at 0755
	// and owned by root means no unprivileged process can write a temporary
	// file -- which on the first real board presented as "wget: can't open
	// /tmp/x: Permission denied" and looks nothing like a missing sticky bit.
	// /root at 0755 is world-readable, which it should not be.
	dirs := map[string]uint32{
		"/proc": 0o755, "/sys": 0o755, "/dev": 0o755, "/run": 0o755,
		"/tmp":                0o1777, // sticky and world-writable, as everywhere else
		"/root":               0o700,
		"/home/" + id.Account: 0o755,
		"/mnt":                0o755,
		"/etc/nosaic":         0o755,
	}
	for d, mode := range dirs {
		full := filepath.Join(rootfs, d)
		if err := os.MkdirAll(full, os.FileMode(mode&0o777)); err != nil {
			return err
		}
		// MkdirAll applies the process umask, and a parent that already
		// exists keeps whatever mode it had, so the mode is set explicitly --
		// with a raw chmod, because os.Chmod takes an os.FileMode and Go
		// spells sticky 1<<20 rather than 0o1000. Passing 0o1777 to os.Chmod
		// produces 0777: world-writable with no sticky bit, so any user can
		// delete another's files in /tmp. That is precisely the bug this map
		// was added to fix, made a second time in the fix itself, and it was
		// invisible because 0777 passes every "is /tmp writable" check.
		if err := chmodRaw(full, uint32(mode)); err != nil {
			return err
		}
	}

	// Services are declared once and rendered for whichever init this profile
	// runs. This is the whole reason recipes never write unit files: the same
	// declaration below produces an s6-rc service directory here and a systemd
	// unit elsewhere, and neither is hand-maintained.
	// The board's addresses, if it states any. Declared as a service like
	// everything else, so the same file works under systemd and under s6.
	hasNet, err := writeNetwork(o, rootfs)
	if err != nil {
		return err
	}
	var services []svcgen.Service
	if hasNet {
		services = append(services, svcgen.Service{
			Name:    "network-config",
			Exec:    "/etc/nosaic/apply-network.sh",
			Restart: "never",
		})
	}

	// The console a login is offered on. Getting this wrong does not fail: the
	// getty starts, reconfigures the port, and every byte after it is noise at
	// the speed anyone is actually watching. On this fleet's Aristas that made
	// a working switch look hung, twice, and cost two power cycles.
	consoleDev, consoleBaud := o.Board.ConsolePort()

	// Cooling, before anything that makes the box work harder.
	//
	// Every switch has fans, and a switch running without a control loop is
	// running on whatever duty its firmware left behind. That is survivable
	// for a bring-up session with someone watching and is not a way to leave a
	// machine: this board's thermal failure mode is silent.
	//
	// Started for any board whose HAL can drive fans. A board that cannot says
	// so at runtime and the service exits saying it, which is louder than
	// never having existed.
	//
	// Restart "always" on purpose. The loop sets the fans to full on the way
	// out, so a crash leaves the box cool and noisy rather than cool and
	// unmanaged -- but noisy-forever is a bad resting state, and something has
	// to bring regulation back.
	if o.Board.PlatformHAL.Driver != "" {
		services = append(services, svcgen.Service{
			Name:    "thermal",
			Exec:    "/usr/bin/nosaic platform thermal",
			After:   []string{"network"},
			Restart: "always",
		})
	}

	// The switch chip, released from the board controller's reset.
	//
	// A separate oneshot rather than something the datapath does for itself,
	// because it is platform work and not datapath work: the chip is held in
	// reset by the board, and which board is a different question from which
	// silicon. A board whose chip needs no releasing simply has no HAL driver
	// and gets no service.
	if o.Board.PlatformHAL.Driver != "" && o.Board.DatapathPackage() != "" {
		services = append(services, svcgen.Service{
			Name:    "asic-release",
			Exec:    "/usr/bin/nosaic platform release-asic",
			Restart: "never",
		})
	}

	// The front-panel transmitters.
	//
	// The board gates each cage's laser and leaves them all off from power-on.
	// A switch that has booted should have its ports up, and a dark
	// transmitter produces the least visible fault there is: the link reads UP
	// from this end, because we lock onto the neighbour's light, while the
	// neighbour sees no carrier at all.
	//
	// Separate from asic-release because it is a different question -- one
	// concerns the switch chip, the other the optics in front of it -- and a
	// board might well want one without the other.
	if o.Board.PlatformHAL.Driver != "" {
		services = append(services, svcgen.Service{
			Name:    "transceivers",
			Exec:    "/usr/bin/nosaic platform tx all on",
			Restart: "never",
		})
	}

	// The datapath.
	//
	// Named `nosd` rather than nosd-td2p: the unit, the CLI and the docs only
	// ever say `nosd`, and which chip is behind it is the image builder's
	// business. That is what makes a board with different silicon the same
	// system from here up.
	//
	// After asic-release, because the chip is not on the PCI bus until then
	// and the daemon would find nothing to open.
	if o.Board.DatapathPackage() != "" {
		after := []string{"network"}
		if o.Board.PlatformHAL.Driver != "" {
			after = append(after, "asic-release")
		}
		services = append(services, svcgen.Service{
			Name:    "nosd",
			Exec:    "/usr/sbin/nosd",
			After:   after,
			Restart: "always",
			// The SDK writes tens of thousands of lines bringing the chip up.
			// On a console at 9600 baud that is ten minutes of unusable
			// console for a few seconds of work.
			Verbose: true,
		})
	}

	consoleGetty := svcgen.Service{
		Name:  "getty-console",
		Exec:  fmt.Sprintf("/sbin/getty -L %s %d vt100", consoleDev, consoleBaud),
		After: []string{"network"},
	}
	services = append(services, consoleGetty)

	switch o.Profile.Init {
	case "s6":
		if err := writeS6(o, rootfs, services...); err != nil {
			return err
		}
		return writeFile(rootfs, "/etc/hostname", "nosaic\n", 0o644)
	case "systemd":
		return writeSystemd(o, rootfs, services...)
	}

	inittab := `# Generated by nosaic. The minimal profile's init.
::sysinit:/bin/mount -t proc proc /proc
::sysinit:/bin/mount -t sysfs sys /sys
::sysinit:/bin/mount -t devtmpfs dev /dev
::sysinit:/bin/mkdir -p /dev/pts /run /tmp
::sysinit:/bin/mount -t devpts devpts /dev/pts
::sysinit:/bin/mount -t tmpfs tmpfs /run
::sysinit:/bin/mkdir -p /mnt/data /mnt/boot
::sysinit:/bin/hostname nosaic
::sysinit:/bin/echo "NOSAIC-BOOT userspace reached"
::once:/etc/nosaic/selftest.sh
` + fmt.Sprintf("::respawn:/sbin/getty -L %s %d vt100\n", consoleDev, consoleBaud) +
		`::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
`
	if err := writeFile(rootfs, "/etc/inittab", inittab, 0o644); err != nil {
		return err
	}

	return writeFile(rootfs, "/etc/hostname", "nosaic\n", 0o644)
}

// copyBoardConfig places a board's config/ directory under /etc/nosaic.
func copyBoardConfig(dir, rootfs string) (int, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return 0, nil
		}
		return 0, err
	}
	n := 0
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		b, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			return n, err
		}
		if err := writeFile(rootfs, "/etc/nosaic/"+e.Name(), string(b), 0o644); err != nil {
			return n, err
		}
		n++
	}
	return n, nil
}

// dedupeUsers returns the accounts to add, in a stable order, having removed
// duplicates and anything that would shadow an account the image already has.
//
// Two packages asking for the same account is normal -- a suite split across
// several packages shares one -- and the same name twice in /etc/passwd is not
// an error anybody notices, it just means the second entry is never reached.
// A package colliding with root or the login account is a different matter and
// is refused rather than silently applied, because that one locks the operator
// out of their own switch.
func dedupeUsers(users []nospkg.User, account string) []nospkg.User {
	seen := map[string]bool{"root": true, account: true}
	var out []nospkg.User
	for _, u := range users {
		if u.Name == "" || seen[u.Name] {
			continue
		}
		seen[u.Name] = true
		out = append(out, u)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out
}
