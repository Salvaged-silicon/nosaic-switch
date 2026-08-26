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
	"text/tabwriter"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
	"github.com/salvaged-silicon/nosaic-switch/internal/check"
	"github.com/salvaged-silicon/nosaic-switch/internal/version"
)

const usage = `nosaic — a network OS for end-of-service-life switches and routers

usage: nosaic <command> [args]

available now
  version              print the build identity
  check                validate the repository against the design invariants
  boards               list board ports and their status

not yet implemented
  pkg build <recipe>   build a package            (M2)
  build <board>        assemble a board's image   (M3)
  upgrade              A/B image upgrade          (M3)
  platform hal         report board sensors       (M6)

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

	case "pkg", "build", "upgrade", "platform":
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
