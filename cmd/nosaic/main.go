// Command nosaic is the NOSaic build tool and on-box CLI.
//
// The same binary runs on the build host and on the switch. Subcommands that
// are not implemented yet say so plainly rather than pretending: NOSaic
// advertises what works, not what is planned.
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"text/tabwriter"

	"github.com/salvaged-silicon/nosaic-switch/internal/arch"
	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/check"
	"github.com/salvaged-silicon/nosaic-switch/internal/imgbuild"
	"github.com/salvaged-silicon/nosaic-switch/internal/nospkg"
	"github.com/salvaged-silicon/nosaic-switch/internal/pkgbuild"
	"github.com/salvaged-silicon/nosaic-switch/internal/profile"
	"github.com/salvaged-silicon/nosaic-switch/internal/recipe"
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

	case "upgrade", "platform":
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
