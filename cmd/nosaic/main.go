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
	"path/filepath"
	"strconv"
	"strings"
	"text/tabwriter"

	"github.com/salvaged-silicon/nosaic-switch/internal/arch"
	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/check"
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
  build <board>                assemble a board's image
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

	case "build":
		if len(args) != 2 {
			fmt.Fprintln(os.Stderr, "usage: nosaic build <board>")
			os.Exit(2)
		}
		if err := buildImage(repoRoot(), args[1]); err != nil {
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

	case "platform":
		fmt.Fprintf(os.Stderr, "nosaic: %q is not implemented yet\n", args[0])
		fmt.Fprintln(os.Stderr, "see docs/DESIGN.md for which milestone lands it")
		os.Exit(3)

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

func buildImage(root, boardID string) error {
	b, err := board.Load(filepath.Join(root, "platform", boardID, "board.yml"))
	if err != nil {
		return err
	}
	a, err := arch.Load(filepath.Join(root, "arch", b.Arch, "arch.yml"))
	if err != nil {
		return err
	}
	pr, err := profile.Load(root, b.Profile)
	if err != nil {
		return err
	}
	res, err := imgbuild.Build(imgbuild.Options{
		Root:       root,
		Board:      b,
		Arch:       a,
		Profile:    pr,
		PackageDir: filepath.Join(root, "out", "packages"),
		OutDir:     filepath.Join(root, "out", "images", boardID),
		Version:    version.Version,
		Log:        os.Stdout,
	})
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
