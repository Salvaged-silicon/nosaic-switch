/* SPDX-License-Identifier: Apache-2.0 */

package main

import (
	"strings"
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/nosd/mem"
	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

func TestRenderRunningConfigAndDiff(t *testing.T) {
	sw := mem.New(mem.Config{
		Ports: 33,
		Caps: switchapi.Capabilities{
			VLANs: true, MaxVLANs: 4094,
			LAGs: true, MaxLAGs: 16, MaxLAGMembers: 8, LACP: true,
			STP: true, MACAging: true,
		},
	})
	if err := sw.AddVLAN(700); err != nil {
		t.Fatal(err)
	}
	if err := sw.AddVLAN(1201); err != nil {
		t.Fatal(err)
	}
	if err := sw.AddLAG("po11", true); err != nil {
		t.Fatal(err)
	}
	if err := sw.SetLAGMembers("po11", []string{"swp1"}); err != nil {
		t.Fatal(err)
	}
	if err := sw.AddLAG("po5", true); err != nil {
		t.Fatal(err)
	}
	if err := sw.SetLAGMembers("po5", []string{"swp29", "swp33"}); err != nil {
		t.Fatal(err)
	}
	if err := sw.SetPortVLAN("po11", 700, true); err != nil {
		t.Fatal(err)
	}
	if err := sw.SetPortVLAN("po11", 1201, true); err != nil {
		t.Fatal(err)
	}
	if err := sw.SetPortVLAN("swp2", 700, false); err != nil {
		t.Fatal(err)
	}
	if err := sw.SetSTP(switchapi.STPConfig{Enabled: true, Priority: 57344}); err != nil {
		t.Fatal(err)
	}

	got, err := renderRunningConfig(sw, []string{"iface eth0 10.60.186.42/16"}, nil)
	if err != nil {
		t.Fatalf("renderRunningConfig: %v", err)
	}
	for _, want := range []string{
		"iface eth0 10.60.186.42/16",
		"lag po11 lacp swp1",
		"lag po5 lacp swp29,swp33",
		"stp on priority 57344",
		"vlan 700",
		"vlan 1201",
		"switchport po11 trunk 700,1201",
		"switchport swp2 access 700",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("rendered config missing %q:\n%s", want, got)
		}
	}

	// Identical saved config produces empty diff.
	if d := diffConfigs(got, got); d != "" {
		t.Errorf("expected empty diff, got:\n%s", d)
	}

	// Removing a line from saved shows '+' in diff; extra saved line shows '-'.
	saved := "iface eth0 10.60.186.42/16\nvlan 999\n"
	d := diffConfigs(saved, got)
	if !strings.Contains(d, "- vlan 999") || !strings.Contains(d, "+ vlan 700") {
		t.Errorf("unexpected diff:\n%s", d)
	}
}
