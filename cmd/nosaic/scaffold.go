package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/board"
)

// Adding a board is meant to be one self-contained directory and no change to
// anything central. That is only true if starting one is easy, and copying
// TEMPLATE by hand is how a port begins with a stale id, a docs page that
// still says TEMPLATE, and a board.yml whose comments describe a different
// switch.
//
// This writes the directory from the same TEMPLATE a contributor would copy,
// with the four axes filled in and the parts that must be per-board left
// blank rather than plausibly wrong.

const scaffoldUsage = `usage: nosaic board scaffold <id> [options]

  <id>            directory name under platform/, e.g. edgecore-as5610-52x

options:
  --vendor NAME   e.g. edgecore, arista
  --model NAME    e.g. as5610-52x
  --arch ARCH     %s
  --asic ID       e.g. td2p, tdp, helix4, prestera, virt
  --boot ID       %s
  --profile P     full | slim | minimal   (default minimal)
  --console DEV,BAUD   e.g. ttyS0,115200

Everything not given is left empty in board.yml for somebody to fill in,
because a plausible guess in a board file is worse than a blank one.
`

func scaffoldCmd(root string, args []string) error {
	if len(args) == 0 || args[0] == "-h" || args[0] == "--help" {
		fmt.Printf(scaffoldUsage, strings.Join(archIDs(root), " | "),
			strings.Join(bootIDs(root), " | "))
		return nil
	}
	id := args[0]
	if id == "" || strings.ContainsAny(id, "/ \t") {
		return fmt.Errorf("board id %q: no spaces or slashes; it is a directory name", id)
	}

	opt := map[string]string{"profile": "minimal"}
	for i := 1; i < len(args); i++ {
		name := strings.TrimPrefix(args[i], "--")
		if name == args[i] || i+1 >= len(args) {
			return fmt.Errorf("unexpected argument %q", args[i])
		}
		i++
		opt[name] = args[i]
	}

	dst := filepath.Join(root, "platform", id)
	if _, err := os.Stat(dst); err == nil {
		return fmt.Errorf("platform/%s already exists", id)
	}

	// Copied from TEMPLATE rather than generated, so the scaffold a
	// contributor gets is the one in the tree and cannot drift from it.
	src := filepath.Join(root, "platform", "TEMPLATE")
	if err := copyTree(src, dst); err != nil {
		return err
	}
	if err := fillBoard(filepath.Join(dst, "board.yml"), id, opt); err != nil {
		return err
	}

	fmt.Printf("platform/%s/ created from TEMPLATE\n", id)
	fmt.Printf("  next: fill in board.yml's notes, then\n")
	fmt.Printf("        nosaic check          -- it will say what is still missing\n")
	fmt.Printf("        nosaic build %s\n", id)
	return nil
}

// fillBoard substitutes the axes into the copied board.yml, leaving anything
// not supplied exactly as TEMPLATE had it.
func fillBoard(path, id string, opt map[string]string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	out := []string{}
	for _, line := range strings.Split(string(b), "\n") {
		for _, k := range []string{"id", "vendor", "model", "arch", "asic", "boot", "profile"} {
			v := opt[k]
			if k == "id" {
				v = id
			}
			if v == "" {
				continue
			}
			if strings.HasPrefix(line, k+":") {
				comment := ""
				if i := strings.Index(line, "#"); i >= 0 {
					comment = "  " + line[i:]
				}
				line = fmt.Sprintf("%s: %s%s", k, v, comment)
			}
		}
		out = append(out, line)
	}
	body := strings.Join(out, "\n")
	if c := opt["console"]; c != "" {
		// --console is spelled DEV,BAUD because that is how the kernel spells
		// it on its command line. board.yml keeps them apart, because getty is
		// handed the device on its own: writing "ttyS0,115200" into console:
		// produces a getty opening a device of that name, which does not
		// exist, and a board that boots to no login at all.
		dev, baud, _ := strings.Cut(c, ",")
		body += fmt.Sprintf("\n# Read off the running switch, not guessed: a wrong baud rate\n"+
			"# makes a working board look hung.\nconsole: %q\n", dev)
		if baud != "" {
			body += fmt.Sprintf("console_baud: %s\n", baud)
		}
	}
	return os.WriteFile(path, []byte(body), 0o644)
}

func copyTree(src, dst string) error {
	return filepath.Walk(src, func(p string, fi os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, p)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)
		if fi.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		b, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		return os.WriteFile(target, b, fi.Mode().Perm())
	})
}

func archIDs(root string) []string { return dirNames(filepath.Join(root, "arch")) }
func bootIDs(root string) []string { return []string{"virt", "aboot", "onie-sfx", "uboot"} }

func dirNames(dir string) []string {
	es, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []string
	for _, e := range es {
		if e.IsDir() {
			out = append(out, e.Name())
		}
	}
	sort.Strings(out)
	return out
}

// listBoardsTo exists so the scaffold's "next" hint and `nosaic boards` agree
// about what a board is.
var _ = io.Discard
var _ = board.LoadAll
