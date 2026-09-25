package imgbuild

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Each source in turn, and the addresses that must never be taken: an
// unprogrammed NIC's all-zero suffix is the same on every unit of the model,
// and a locally administered one is what the fallback already is.
func TestSwitchMACSources(t *testing.T) {
	cases := []struct {
		name, board, conf, eth0, want string
	}{
		{"the board says", "b4:de:31:3f:a5:c0", "iface eth0 10.0.0.1/24 mac 44:4c:a8:eb:93:f6\n", "", "b4:de:31:3f:a5:c0"},
		{"network.conf, when the board cannot", "", "iface eth0 10.0.0.1/24 mac 44:4C:A8:EB:93:F6\n", "", "44:4c:a8:eb:93:f6"},
		{"mac auto is not an address", "", "iface eth0 10.0.0.1/24 mac auto\n", "80:a2:35:81:ca:ae", "80:a2:35:81:ca:ae"},
		{"an unprogrammed NIC is refused", "", "iface eth0 10.0.0.1/24\n", "00:a0:c9:00:00:00", ""},
		{"a locally administered one is refused", "", "", "02:11:22:33:44:55", ""},
		{"the board's wins over the config", "80:a2:35:81:ca:ae", "iface eth0 10.0.0.1/24 mac 00:1c:73:da:fe:7a\n", "", "80:a2:35:81:ca:ae"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			dir := t.TempDir()
			bin := filepath.Join(dir, "bin")
			if err := os.Mkdir(bin, 0o755); err != nil {
				t.Fatal(err)
			}
			fake := "#!/bin/sh\nexit 1\n"
			if c.board != "" {
				fake = "#!/bin/sh\n[ \"$1 $2\" = \"platform mac\" ] && echo " + c.board + " && exit 0\nexit 1\n"
			}
			write := func(p, s string, mode os.FileMode) {
				if err := os.WriteFile(p, []byte(s), mode); err != nil {
					t.Fatal(err)
				}
			}
			write(filepath.Join(bin, "nosaic"), fake, 0o755)
			write(filepath.Join(dir, "network.conf"), c.conf, 0o644)
			write(filepath.Join(dir, "eth0"), c.eth0+"\n", 0o644)
			write(filepath.Join(dir, "switch-mac.sh"), switchMAC, 0o755)

			out := filepath.Join(dir, "run", "base_mac")
			cmd := exec.Command("sh", filepath.Join(dir, "switch-mac.sh"))
			cmd.Env = append(os.Environ(), "PATH="+bin+":"+os.Getenv("PATH"),
				"NOSAIC_MAC_OUT="+out,
				"NOSAIC_NET_CONF="+filepath.Join(dir, "network.conf"),
				"NOSAIC_ETH0_ADDR="+filepath.Join(dir, "eth0"))
			log, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("%v\n%s", err, log)
			}
			got, _ := os.ReadFile(out)
			if strings.TrimSpace(string(got)) != c.want {
				t.Errorf("base_mac = %q, want %q\n%s", strings.TrimSpace(string(got)), c.want, log)
			}
		})
	}
}
