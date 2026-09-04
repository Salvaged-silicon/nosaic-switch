// Command nosaic is the NOSaic build tool and on-box CLI.
//
// The same binary runs on the build host and on the switch. Subcommands that
// are not implemented yet say so plainly rather than pretending: NOSaic
// advertises what works, not what is planned.
package main

import (
	"flag"
	"fmt"
	"net/netip"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"text/tabwriter"
	"unsafe"

	"github.com/salvaged-silicon/nosaic-switch/internal/arch"
	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/boot"
	"github.com/salvaged-silicon/nosaic-switch/internal/check"
	"github.com/salvaged-silicon/nosaic-switch/internal/config"
	"github.com/salvaged-silicon/nosaic-switch/internal/depsolve"
	"github.com/salvaged-silicon/nosaic-switch/internal/docsgen"
	"github.com/salvaged-silicon/nosaic-switch/internal/imgbuild"
	nosdclient "github.com/salvaged-silicon/nosaic-switch/internal/nosd/client"
	"github.com/salvaged-silicon/nosaic-switch/internal/nospkg"
	"github.com/salvaged-silicon/nosaic-switch/internal/pkgbuild"
	"github.com/salvaged-silicon/nosaic-switch/internal/profile"
	"github.com/salvaged-silicon/nosaic-switch/internal/recipe"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
	"github.com/salvaged-silicon/nosaic-switch/internal/upgrade"
	"github.com/salvaged-silicon/nosaic-switch/internal/version"
)

const usage = `nosaic — a network OS for end-of-service-life switches and routers

usage: nosaic <command> [args]

available now
  version                      print the build identity
  check                        validate the repository against the invariants
  boards                       list board ports and their status
  pkg build <name> --arch A    build a package from its recipe
  pkg info <file.nos>          show a package's manifest
  pkg verify <file.nos>        re-derive every digest in a package
  pkg order [--profile P]      list recipes in dependency order
  build [board]                assemble a board's image; lists boards if omitted
  upgrade status <disk>        show which slot is active or on trial
  upgrade install <disk> <img> --slot b   install into the inactive slot

on a running switch
  show ports | routes | caps    what the datapath is doing
  interface <name> up|down      administrative state
  interface <name> mtu <n>      set the MTU
  route add <prefix> via <ip> dev <port>
  route del <prefix>

not yet implemented
  upgrade                      A/B image upgrade          (M3)
  platform hal                 report board sensors       (M6)

`

func main() {
	flag.Usage = func() { fmt.Fprint(os.Stderr, usage) }
	flag.Parse()

	args := flag.Args()
	if len(args) == 0 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}

	switch args[0] {
	case "version":
		fmt.Printf("nosaic %s (%s)\n", version.Version, version.Commit)

	case "check":
		root := repoRoot()
		if !check.Run(root).Report(os.Stdout) {
			os.Exit(1)
		}

	case "board":
		if len(args) < 2 || args[1] != "scaffold" {
			fmt.Fprintln(os.Stderr, "usage: nosaic board scaffold <id> [options]")
			os.Exit(2)
		}
		if err := scaffoldCmd(repoRoot(), args[2:]); err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}

	case "boards":
		if err := listBoards(repoRoot()); err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}

	case "pkg":
		if err := pkgCmd(repoRoot(), args[1:]); err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}

	case "docs":
		if len(args) != 2 || args[1] != "index" {
			fmt.Fprintln(os.Stderr, "usage: nosaic docs index")
			os.Exit(2)
		}
		root := repoRoot()
		boards, err := board.LoadAll(root)
		if err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}
		if err := docsgen.Write(root, boards); err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("wrote %s\n", docsgen.Path)

	case "build":
		root := repoRoot()
		target := ""
		profileOverride := ""
		ramBoot := false
		rest := args[1:]
		for i := 0; i < len(rest); i++ {
			switch {
			case rest[i] == "--profile" && i+1 < len(rest):
				profileOverride = rest[i+1]
				i++
			case strings.HasPrefix(rest[i], "--profile="):
				profileOverride = strings.TrimPrefix(rest[i], "--profile=")
			case rest[i] == "--ram-boot":
				ramBoot = true
			default:
				target = rest[i]
			}
		}
		if target == "" {
			// No board named. Rather than an unhelpful usage line, show what
			// there is to choose from -- and offer to choose, but only when a
			// person is actually at a terminal. Prompting in a script or in CI
			// would hang a build waiting for input nobody is there to give.
			chosen, err := chooseBoard(root)
			if err != nil {
				fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
				os.Exit(2)
			}
			target = chosen
		}
		if err := buildImage(root, target, profileOverride, ramBoot); err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}

	case "upgrade":
		if err := upgradeCmd(args[1:]); err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}

	case "show", "interface", "route":
		if err := switchCmd(args); err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}

	case "config":
		if err := configCmd(args[1:]); err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}

	case "platform":
		if err := platformCmd(args[1:]); err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}

	default:
		fmt.Fprintf(os.Stderr, "nosaic: unknown command %q\n\n", args[0])
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
}

func listBoards(root string) error {
	boards, err := board.LoadAll(root)
	if err != nil {
		return err
	}
	if len(boards) == 0 {
		fmt.Println("No board ports yet.")
		fmt.Println("The first is virt-x86_64, landing at M3 — see docs/DESIGN.md.")
		return nil
	}
	w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
	fmt.Fprintln(w, "BOARD\tARCH\tASIC\tBOOT\tPROFILE\tSTATUS")
	for _, b := range boards {
		fmt.Fprintf(w, "%s\t%s\t%s\t%s\t%s\t%s\n", b.ID, b.Arch, b.ASIC, b.Boot, b.Profile, b.Status)
	}
	return w.Flush()
}

// repoRoot walks up from the working directory looking for go.mod. On a
// switch there is no repository, but the commands that need one are all
// build-host commands.
func repoRoot() string {
	dir, err := os.Getwd()
	if err != nil {
		return "."
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "go.mod")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "."
		}
		dir = parent
	}
}

func pkgCmd(root string, args []string) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: nosaic pkg <build|info|verify> ...")
	}
	switch args[0] {
	case "docs":
		if len(args) != 2 || args[1] != "index" {
			fmt.Fprintln(os.Stderr, "usage: nosaic docs index")
			os.Exit(2)
		}
		root := repoRoot()
		boards, err := board.LoadAll(root)
		if err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}
		if err := docsgen.Write(root, boards); err != nil {
			fmt.Fprintf(os.Stderr, "nosaic: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("wrote %s\n", docsgen.Path)

	case "build":
		// The package name is positional and comes first, because Go's flag
		// package stops parsing at the first non-flag argument -- so flags
		// written after a positional would be silently ignored.
		if len(args) < 2 || strings.HasPrefix(args[1], "-") {
			return fmt.Errorf("usage: nosaic pkg build <name> --arch <arch>")
		}
		name := args[1]
		fs := flag.NewFlagSet("pkg build", flag.ExitOnError)
		archID := fs.String("arch", "", "target architecture")
		jobs := fs.Int("jobs", 1, "parallel make jobs")
		out := fs.String("out", "out/packages", "output directory")
		epoch := fs.Int64("epoch", 0, "SOURCE_DATE_EPOCH (0 = the project default)")
		if err := fs.Parse(args[2:]); err != nil {
			return err
		}
		if *archID == "" {
			return fmt.Errorf("--arch is required")
		}
		return pkgBuild(root, name, *archID, *jobs, *out, *epoch)

	case "order":
		fs := flag.NewFlagSet("pkg order", flag.ExitOnError)
		prof := fs.String("profile", "", "only what this profile needs")
		if err := fs.Parse(args[1:]); err != nil {
			return err
		}
		_ = prof
		// Build order, computed rather than assumed. `make packages` used to
		// build in directory order, which is alphabetical and therefore wrong
		// the moment one package needs another: systemd before libcap, s6
		// before skalibs. The resolver already knows the answer.
		return pkgOrder(root, *prof)

	case "info", "verify":
		if len(args) != 2 {
			return fmt.Errorf("usage: nosaic pkg %s <file.nos>", args[0])
		}
		var (
			m   *nospkg.Manifest
			err error
		)
		if args[0] == "verify" {
			m, err = nospkg.VerifyFile(args[1])
		} else {
			m, err = nospkg.ReadManifestFile(args[1])
		}
		if err != nil {
			return err
		}
		printManifest(m, args[0] == "verify")
		return nil
	}
	return fmt.Errorf("unknown pkg subcommand %q", args[0])
}

func pkgBuild(root, name, archID string, jobs int, out string, epoch int64) error {
	r, err := recipe.Load(filepath.Join(root, "recipes", name, "recipe.yml"))
	if err != nil {
		return err
	}
	a, err := arch.Load(filepath.Join(root, "arch", archID, "arch.yml"))
	if err != nil {
		return err
	}
	res, err := pkgbuild.Build(pkgbuild.Options{
		Root:   root,
		Recipe: r,
		Arch:   a,
		Jobs:   jobs,
		OutDir: filepath.Join(root, out),
		Epoch:  epoch,
		Log:    os.Stdout,
	})
	if err != nil {
		return err
	}
	printManifest(res.Manifest, false)
	return nil
}

func printManifest(m *nospkg.Manifest, verified bool) {
	w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
	fmt.Fprintf(w, "name\t%s\n", m.Name)
	fmt.Fprintf(w, "version\t%s\n", m.Version)
	fmt.Fprintf(w, "arch\t%s\n", m.Arch)
	fmt.Fprintf(w, "license\t%s\n", m.License)
	fmt.Fprintf(w, "redistributable\t%v\n", m.Redistributable)
	if len(m.Provides) > 0 {
		fmt.Fprintf(w, "provides\t%s\n", strings.Join(m.Provides, ", "))
	}
	if len(m.Depends) > 0 {
		fmt.Fprintf(w, "depends\t%s\n", strings.Join(m.Depends, ", "))
	}
	fmt.Fprintf(w, "files\t%d\n", len(m.Files))
	fmt.Fprintf(w, "payload sha256\t%s\n", m.PayloadSHA256)
	if verified {
		fmt.Fprintf(w, "verified\tevery digest re-derived and matched\n")
	}
	w.Flush()
}

// buildImage assembles a board's image. profileOverride is empty for a normal
// build, where the board's own profile is used.
//
// The override exists so a tier can be built and booted for a board that does
// not declare it. Without it there was no way to test the systemd tiers at
// all, and no way for CI to build every profile -- which the design names as
// the thing keeping the abstract services: stanza honest, since a recipe
// reaching for a systemd-specific feature breaks the s6 tier first.
func buildImage(root, boardID, profileOverride string, ramBoot bool) error {
	b, err := board.Load(filepath.Join(root, "platform", boardID, "board.yml"))
	if err != nil {
		return err
	}
	a, err := arch.Load(filepath.Join(root, "arch", b.Arch, "arch.yml"))
	if err != nil {
		return err
	}
	want := b.Profile
	if profileOverride != "" {
		want = profileOverride
	}
	pr, err := profile.Load(root, want)
	if err != nil {
		return err
	}
	res, err := imgbuild.Build(imgbuild.Options{
		Root:       root,
		Board:      b,
		Arch:       a,
		Profile:    pr,
		RAMBoot:    ramBoot,
		PackageDir: filepath.Join(root, "out", "packages"),
		OutDir:     filepath.Join(root, "out", "images", boardID),
		Version:    version.Version,
		Log:        os.Stdout,
	})
	if err != nil {
		return err
	}
	// The installable artifact for whatever bootloader this board has. Every
	// board goes through a backend, including the virtual one -- the moment a
	// board is special-cased here, the interface stops being what every board
	// uses.
	backend, err := boot.For(b.Boot)
	if err != nil {
		return err
	}
	dtb, err := compileDeviceTree(root, b, filepath.Join(root, "out", "images", boardID))
	if err != nil {
		return err
	}
	// Built BEFORE the installer is wrapped, because an installer for a
	// U-Boot board has to carry the FIT: that firmware loads a raw partition,
	// not a filesystem, so the boot image is placed at install time rather
	// than found at boot.
	//
	// A board installed by ONIE still has U-Boot underneath it, and U-Boot can
	// load a FIT over the network into RAM. That is the only way to try an
	// image on such a board without writing its disk -- and on a board that
	// shares one disk with ONIE, writing the disk is what removes the way
	// back. So the FIT is built for any board that states U-Boot addresses,
	// not only for boards whose installer is U-Boot.
	netboot := ""
	if b.UBootArch != "" && b.Boot != "uboot" {
		fit, err := boot.For("uboot")
		if err != nil {
			return err
		}
		netboot, err = fit.Wrap(boot.Image{
			Kernel: res.Kernel, Initramfs: res.Initramfs,
			Board: b.ID, Arch: a.ID, Version: version.Version,
			DTB:       dtb,
			UBootArch: b.UBootArch, UBootLoad: b.UBootLoad, UBootEntry: b.UBootEntry,
			UBootStage: b.UBootStage, Console: consoleArg(b),
			FDTAddr: b.UBootFDTAddr, RamdiskAddr: b.UBootRamdiskAddr,
			FITHash:      b.UBootFITHash,
			KernelParams: b.KernelParams,
		}, filepath.Join(root, "out", "images", boardID), os.Stdout)
		if err != nil {
			return err
		}
	}

	artifact, err := backend.Wrap(boot.Image{
		Kernel: res.Kernel, Initramfs: res.Initramfs,
		Squashfs: res.Squashfs, Disk: res.Disk,
		FIT: netboot, FITOffset: res.FITOffset, NOSBootCmd: b.UBootNOSBootCmd,
		Board: b.ID, Arch: a.ID, Version: version.Version,
		DTB:       dtb,
		UBootArch: b.UBootArch, UBootLoad: b.UBootLoad, UBootEntry: b.UBootEntry,
		UBootStage: b.UBootStage, Console: consoleArg(b),
		FDTAddr: b.UBootFDTAddr, RamdiskAddr: b.UBootRamdiskAddr,
		FITHash:         b.UBootFITHash,
		AbootMaxHWEpoch: b.AbootMaxHWEpoch,
		KernelParams:    b.KernelParams,
	}, filepath.Join(root, "out", "images", boardID), os.Stdout)
	if err != nil {
		return err
	}

	fmt.Printf("\nimage for %s (%s profile)\n", b.ID, pr.Name)
	for _, p := range res.Packages {
		fmt.Printf("  %s\n", p)
	}
	for _, f := range []string{res.Kernel, res.Initramfs, res.Squashfs} {
		if fi, err := os.Stat(f); err == nil {
			fmt.Printf("  %-42s %6.1f MiB\n", f, float64(fi.Size())/(1<<20))
		}
	}
	fmt.Printf("\ninstall with %s\n  %s\n", backend.ID(), backend.Describe())
	if fi, err := os.Stat(artifact); err == nil {
		fmt.Printf("  %-42s %6.1f MiB\n", artifact, float64(fi.Size())/(1<<20))
	}
	if netboot != "" {
		fmt.Printf("\nor try it without installing, from the U-Boot prompt\n")
		stage := b.UBootStage
		if stage == "" {
			stage = "0x02000000"
		}
		fmt.Printf("  setenv bootargs 'console=%s'\n", consoleArg(b))
		fmt.Printf("  tftpboot %s %s && bootm %s#nosaic\n", stage, filepath.Base(netboot), stage)
		if fi, err := os.Stat(netboot); err == nil {
			fmt.Printf("  %-42s %6.1f MiB\n", netboot, float64(fi.Size())/(1<<20))
		}
	}
	return nil
}

func upgradeCmd(args []string) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: nosaic upgrade <status|install> <disk> ...")
	}
	switch args[0] {
	case "status":
		if len(args) != 2 {
			return fmt.Errorf("usage: nosaic upgrade status <disk>")
		}
		st, err := upgrade.Status(upgrade.Disk{Path: args[1]})
		if err != nil {
			return err
		}
		w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
		fmt.Fprintf(w, "active\t%s\n", st.Active)
		if st.Trial != "" {
			fmt.Fprintf(w, "trial\t%s (attempt %d)\n", st.Trial, st.Tries)
			fmt.Fprintf(w, "\tnot yet committed: it rolls back unless it confirms itself healthy\n")
		} else {
			fmt.Fprintf(w, "trial\tnone\n")
		}
		return w.Flush()

	case "install":
		if len(args) < 3 {
			return fmt.Errorf("usage: nosaic upgrade install <disk> <image> --slot <a|b>")
		}
		disk, image := args[1], args[2]
		fs := flag.NewFlagSet("upgrade install", flag.ExitOnError)
		slot := fs.String("slot", "", "slot to install into (must not be the active one)")
		if err := fs.Parse(args[3:]); err != nil {
			return err
		}
		if *slot == "" {
			return fmt.Errorf("--slot is required")
		}
		if err := upgrade.Install(upgrade.Disk{Path: disk}, *slot, image); err != nil {
			return err
		}
		fmt.Printf("installed %s into slot %s, marked for trial\n", filepath.Base(image), *slot)
		fmt.Println("it becomes active only after it boots and confirms itself healthy")
		return nil
	}
	return fmt.Errorf("unknown upgrade subcommand %q", args[0])
}

// switchCmd handles the commands that talk to a running datapath.
//
// They are written against switchapi, not against nosd, so the same code would
// work against an in-process datapath. What a user types does not depend on
// which chip is underneath — that is the whole point of the contract.
func switchCmd(args []string) error {
	c, err := nosdclient.Dial(os.Getenv("NOSD_SOCKET"))
	if err != nil {
		return err
	}
	defer c.Close()

	switch args[0] {
	case "show":
		if len(args) < 2 {
			return fmt.Errorf("usage: nosaic show <ports|routes|caps>")
		}
		return showCmd(c, args[1])

	case "interface":
		if len(args) < 3 {
			return fmt.Errorf("usage: nosaic interface <name> <up|down|mtu <n>>")
		}
		name := args[1]
		switch args[2] {
		case "up":
			return c.SetPortAdmin(name, true)
		case "down":
			return c.SetPortAdmin(name, false)
		case "mtu":
			if len(args) < 4 {
				return fmt.Errorf("usage: nosaic interface %s mtu <n>", name)
			}
			mtu, err := strconv.Atoi(args[3])
			if err != nil {
				return fmt.Errorf("mtu %q is not a number", args[3])
			}
			return c.SetPortMTU(name, mtu)
		}
		return fmt.Errorf("unknown interface command %q", args[2])

	case "route":
		return routeCmd(c, args[1:])
	}
	return fmt.Errorf("unknown command %q", args[0])
}

func showCmd(c *nosdclient.Client, what string) error {
	w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
	defer w.Flush()

	switch what {
	case "caps":
		caps := c.Capabilities()
		fmt.Fprintf(w, "driver\t%s\n", caps.Driver)
		fmt.Fprintf(w, "contract\t%s\n", caps.Contract)
		fmt.Fprintf(w, "ports\t%d max\n", caps.MaxPorts)
		fmt.Fprintf(w, "vlans\t%v\n", caps.VLANs)
		fmt.Fprintf(w, "l3\t%v\n", caps.L3)
		// Reported explicitly because an operator planning multipath needs to
		// know before configuring it, not after a route is refused.
		if caps.ECMP {
			fmt.Fprintf(w, "ecmp\tyes, up to %d paths\n", caps.MaxECMP)
		} else {
			fmt.Fprintf(w, "ecmp\tno\n")
		}
		return nil

	case "ports":
		ports, err := c.Ports()
		if err != nil {
			return err
		}
		fmt.Fprintln(w, "PORT\tADMIN\tOPER\tSPEED\tMTU")
		for _, p := range ports {
			st, err := c.PortStatus(p.Name)
			if err != nil {
				return err
			}
			fmt.Fprintf(w, "%s\t%s\t%s\t%d\t%d\n",
				st.Name, updown(st.AdminUp), updown(st.OperUp), st.SpeedMbps, st.MTU)
		}
		return nil

	case "routes":
		routes, err := c.Routes()
		if err != nil {
			return err
		}
		if len(routes) == 0 {
			fmt.Fprintln(w, "no routes")
			return nil
		}
		fmt.Fprintln(w, "PREFIX\tNEXT-HOPS")
		for _, r := range routes {
			var hops []string
			for _, nh := range r.NextHops {
				hops = append(hops, nh.Via.String()+" dev "+nh.Port)
			}
			fmt.Fprintf(w, "%s\t%s\n", r.Prefix, strings.Join(hops, ", "))
		}
		return nil
	}
	return fmt.Errorf("unknown show target %q", what)
}

func routeCmd(c *nosdclient.Client, args []string) error {
	if len(args) < 2 {
		return fmt.Errorf("usage: nosaic route add <prefix> via <ip> dev <port> | route del <prefix>")
	}
	prefix, err := netip.ParsePrefix(args[1])
	if err != nil {
		return fmt.Errorf("%q is not a prefix: %w", args[1], err)
	}

	switch args[0] {
	case "del":
		return c.DelRoute(prefix)

	case "add":
		// Repeating "via ... dev ..." adds a next-hop, which is how a
		// multipath route is expressed. If the datapath cannot do multipath it
		// refuses, rather than installing the first and saying nothing.
		r := switchapi.Route{Prefix: prefix}
		rest := args[2:]
		for len(rest) >= 4 && rest[0] == "via" && rest[2] == "dev" {
			via, err := netip.ParseAddr(rest[1])
			if err != nil {
				return fmt.Errorf("%q is not an address: %w", rest[1], err)
			}
			r.NextHops = append(r.NextHops, switchapi.NextHop{Via: via, Port: rest[3]})
			rest = rest[4:]
		}
		if len(rest) != 0 {
			return fmt.Errorf("unexpected %q: expected via <ip> dev <port>", strings.Join(rest, " "))
		}
		if len(r.NextHops) == 0 {
			return fmt.Errorf("a route needs at least one next-hop: via <ip> dev <port>")
		}
		return c.AddRoute(r)
	}
	return fmt.Errorf("unknown route command %q", args[0])
}

func updown(b bool) string {
	if b {
		return "up"
	}
	return "down"
}

// pkgOrder prints every recipe in an order where each package is built after
// everything it depends on.
func pkgOrder(root, prof string) error {
	recipes, err := recipe.LoadAll(root)
	if err != nil {
		return err
	}
	var available []depsolve.Pkg
	var roots []string
	for _, r := range recipes {
		available = append(available, depsolve.Pkg{
			Name: r.Name, Version: r.Version,
			Provides: r.Provides, Conflicts: r.Conflicts, Depends: r.Depends,
		})
		roots = append(roots, r.Name)
	}

	// Narrowed to one profile's closure when asked. Everything in recipes/ is
	// not necessarily wanted in every image, and something being unfinished
	// should not stop a profile that does not use it from being built.
	if prof != "" {
		pr, err := profile.Load(root, prof)
		if err != nil {
			return err
		}
		roots = pr.Packages
	}
	order, err := depsolve.Resolve(available, roots)
	if err != nil {
		return err
	}
	for _, p := range order {
		fmt.Println(p.Name)
	}
	return nil
}

// isTerminal reports whether f is a terminal.
//
// Checking for a character device is not enough, and fails in the exact case
// this needs to get right: /dev/null is a character device, so `nosaic build
// </dev/null` would have been treated as a person sitting at a console and
// prompted into the void. Asking the terminal driver a question only a
// terminal can answer is the reliable test.
func isTerminal(f *os.File) bool {
	var termios [64]byte
	_, _, errno := syscall.Syscall6(syscall.SYS_IOCTL, f.Fd(),
		syscall.TCGETS, uintptr(unsafe.Pointer(&termios[0])), 0, 0, 0)
	return errno == 0
}

// chooseBoard lists the supported switches and, at a terminal, lets one be
// picked.
//
// The alternative — asking during every build — was considered and rejected:
// a build that blocks on input cannot run in CI or in a script, and the board
// already records which bootloader it uses, so asking would duplicate data
// that is already written down.
func chooseBoard(root string) (string, error) {
	boards, err := board.LoadAll(root)
	if err != nil {
		return "", err
	}
	if len(boards) == 0 {
		return "", fmt.Errorf("no boards are supported yet; see platform/TEMPLATE to add one")
	}

	w := tabwriter.NewWriter(os.Stderr, 0, 0, 2, ' ', 0)
	fmt.Fprintln(os.Stderr, "Which switch?")
	fmt.Fprintln(w, "\t\tBOARD\tINSTALLS BY\tSTATUS")
	for i, b := range boards {
		how := b.Boot
		if be, err := boot.For(b.Boot); err == nil {
			how = be.Describe()
		}
		fmt.Fprintf(w, "\t%d.\t%s\t%s\t%s\n", i+1, b.ID, how, b.Status)
	}
	w.Flush()

	// A terminal means a person; anything else means a script.
	if !isTerminal(os.Stdin) {
		return "", fmt.Errorf("name one: nosaic build <board>")
	}

	fmt.Fprint(os.Stderr, "\nnumber or name: ")
	var answer string
	if _, err := fmt.Fscanln(os.Stdin, &answer); err != nil {
		return "", fmt.Errorf("nothing chosen")
	}
	answer = strings.TrimSpace(answer)
	if n, err := strconv.Atoi(answer); err == nil {
		if n < 1 || n > len(boards) {
			return "", fmt.Errorf("%d is not one of the %d boards listed", n, len(boards))
		}
		return boards[n-1].ID, nil
	}
	for _, b := range boards {
		if b.ID == answer {
			return b.ID, nil
		}
	}
	return "", fmt.Errorf("no board called %q", answer)
}

// compileDeviceTree builds the board's .dts into a .dtb, if it has one.
//
// Compiling here rather than shipping a checked-in .dtb keeps the reviewable
// artifact the source: a device tree is the one board file where a wrong value
// produces a board that hangs before the console opens, so it needs to be
// readable in a diff. dtc is already a build dependency -- mkimage shells out
// to it to compile the FIT description.
func compileDeviceTree(root string, b *board.Board, outDir string) (string, error) {
	if b.DeviceTree == "" {
		return "", nil
	}
	src := filepath.Join(root, "platform", b.ID, b.DeviceTree)
	if _, err := os.Stat(src); err != nil {
		return "", fmt.Errorf("board %s declares device_tree %s: %w", b.ID, b.DeviceTree, err)
	}
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return "", err
	}
	out := filepath.Join(outDir, strings.TrimSuffix(filepath.Base(src), ".dts")+".dtb")

	fmt.Printf("==> compiling %s\n", b.DeviceTree)
	// -@ keeps the symbol table, which costs a little space and makes the
	// result usable with overlays later. Warnings are left on: a device tree
	// that dtc grumbles about is usually one that is about to misbehave.
	cmd := exec.Command("dtc", "-I", "dts", "-O", "dtb", "-o", out, src)
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return "", fmt.Errorf("compiling %s: %w", src, err)
	}
	return out, nil
}

// consoleArg renders the console the way a kernel command line spells it.
//
// board.yml keeps the device and the speed apart because getty is handed the
// device alone; bootargs wants them together. Composing here rather than
// storing the joined form keeps one spelling authoritative.
func consoleArg(b *board.Board) string {
	dev, baud := b.ConsolePort()
	return fmt.Sprintf("%s,%d", dev, baud)
}

/*
configCmd reads and edits the switch's configuration files.

It never talks to the datapath. That is deliberate and it is the whole point of
the split: configuration is a document describing what this switch should be,
and something else renders it into running state. A CLI that both edited the
document and poked the chip would let the two disagree, which is exactly the
class of fault the "is it in the ASIC" view exists to catch.

Saying which layer each setting came from is the other half. A switch behaving
unlike its neighbour is nearly always one overridden setting, and without the
source an operator has to know the layering rules and read both directories by
hand.
*/
func configCmd(args []string) error {
	c, err := config.Load()
	if err != nil {
		return err
	}
	sub := "show"
	if len(args) > 0 {
		sub = args[0]
	}

	switch sub {
	case "show":
		pat := ""
		if len(args) > 1 {
			pat = args[1]
		}
		w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
		n := 0
		for _, s := range c.Settings() {
			if pat != "" && !strings.Contains(s.Name, pat) {
				continue
			}
			src := "image"
			if s.Site {
				src = "switch"
			}
			fmt.Fprintf(w, "%s\t%s\t%s\n", s.Name, s.Value, src)
			n++
		}
		w.Flush()
		if n == 0 {
			fmt.Println("no settings" + map[bool]string{true: " match", false: ""}[pat != ""])
			return nil
		}
		fmt.Printf("\n%d setting(s). \"switch\" comes from %s and survives an "+
			"upgrade; \"image\" is the default this image shipped.\n",
			n, config.SiteDir)
		return nil

	case "files":
		fmt.Printf("%-24s the image's defaults, replaced by every upgrade\n", config.ImageDir)
		fmt.Printf("%-24s this switch's own, and it wins\n", config.SiteDir)
		if config.RAMBooted() {
			fmt.Printf("\nThis board has NO DATA PARTITION -- %s is a tmpfs.\n",
				config.SiteDir)
			fmt.Printf("`config set` writes nosaic/config/%s in the bootloader's "+
				"flash, which survives a reboot and an image replacement, and "+
				"the initramfs copies it back into %s at boot.\n",
				config.SiteFile, config.SiteDir)
		} else {
			fmt.Printf("\n`config set` writes %s only.\n",
				filepath.Join(config.SiteDir, config.SiteFile))
		}
		return nil

	case "get":
		if len(args) < 2 {
			return fmt.Errorf("usage: nosaic config get <name>")
		}
		v, ok := c.Get(args[1])
		if !ok {
			return fmt.Errorf("%s is not set", args[1])
		}
		fmt.Println(v)
		return nil

	case "set":
		if len(args) < 3 {
			return fmt.Errorf("usage: nosaic config set <name> <value>")
		}
		if err := config.Set(args[1], &args[2]); err != nil {
			return err
		}
		if config.RAMBooted() {
			// Saying /mnt/data here would be true and useless: it is a
			// tmpfs on this board, and what makes the setting survive is
			// the copy in the bootloader's flash.
			fmt.Printf("%s=%s written to the bootloader's flash "+
				"(nosaic/config/%s)\n", args[1], args[2], config.SiteFile)
			fmt.Println("This board has no data partition, so that is where its " +
				"configuration lives; the initramfs restores it at boot.")
		} else {
			fmt.Printf("%s=%s written to %s\n", args[1], args[2],
				filepath.Join(config.SiteDir, config.SiteFile))
		}
		fmt.Println("It takes effect when the thing that reads it restarts.")
		return nil

	case "unset":
		if len(args) < 2 {
			return fmt.Errorf("usage: nosaic config unset <name>")
		}
		if err := config.Set(args[1], nil); err != nil {
			return err
		}
		fmt.Printf("%s removed; the image's default applies again\n", args[1])
		return nil
	}
	return fmt.Errorf("unknown config command %q", sub)
}
