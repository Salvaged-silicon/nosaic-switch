package recipe

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Every shell script a recipe installs must parse.
//
// ⚠ A SYNTAX ERROR HERE COSTS THE BOOT, NOT THE FEATURE. Scripts installed by
// recipes are run as s6 services, and this init treats a non-zero result from
// bringing the service database up as "the service database did not come up"
// and answers with a rescue shell -- no network, no datapath. A script that
// fails to parse exits non-zero before it does anything, so a missing `fi`
// takes the switch off the network on the next boot and gives no clue why.
//
// The Go compiler cannot see inside these files and neither can the recipe
// loader. dash is the closest thing on a build host to the busybox ash that
// runs them.
func TestInstalledShellScriptsParse(t *testing.T) {
	sh, err := exec.LookPath("dash")
	if err != nil {
		if sh, err = exec.LookPath("sh"); err != nil {
			t.Skip("no POSIX shell available to check with")
		}
	}

	root := filepath.Join("..", "..", "recipes")
	var checked int
	err = filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() || info.Size() == 0 {
			return nil
		}
		// Recipes keep the files they install under nosaic/.
		if !strings.Contains(filepath.ToSlash(path), "/nosaic/") {
			return nil
		}
		b, err := os.ReadFile(path)
		if err != nil {
			return nil
		}
		// By shebang, not by extension: these are installed under names with
		// no suffix, which is what the service exec line refers to.
		if !strings.HasPrefix(string(b), "#!/bin/sh") && !strings.HasPrefix(string(b), "#!/bin/bash") {
			return nil
		}
		checked++
		cmd := exec.Command(sh, "-n", path)
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Errorf("%s is not valid shell, and a recipe installs it as a service:\n%v\n%s",
				path, err, out)
		}
		return nil
	})
	if err != nil {
		t.Fatalf("walking %s: %v", root, err)
	}
	if checked == 0 {
		t.Skip("no installed shell scripts found to check")
	}
	t.Logf("checked %d installed shell script(s)", checked)
}
