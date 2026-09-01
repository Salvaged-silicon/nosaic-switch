package pkgbuild

import (
	"testing"

	"github.com/salvaged-silicon/nosaic-switch/internal/recipe"
)

// A package that creates a system account and installs a 0640 config for it
// must be able to give that account the file. Without this the daemon starts,
// reads nothing, and reports a configuration error rather than a permission
// one -- which is how the FRR OSPF config was silently unreadable.
func TestResolveOwner(t *testing.T) {
	r := &recipe.Recipe{Users: []recipe.User{
		{Name: "frr", UID: 101, GID: 101},
		{Name: "frrvty", UID: 102, GID: 102},
	}}

	for _, tc := range []struct {
		owner    string
		uid, gid int
		wantErr  bool
	}{
		{owner: "", uid: 0, gid: 0},
		{owner: "frr", uid: 101, gid: 101},
		{owner: "frr:frr", uid: 101, gid: 101},
		{owner: "frr:frrvty", uid: 101, gid: 102},
		{owner: "nobody", wantErr: true},
		{owner: "frr:nogroup", wantErr: true},
	} {
		uid, gid, err := resolveOwner(r, tc.owner)
		if tc.wantErr {
			if err == nil {
				t.Errorf("owner %q: expected an error, got %d:%d", tc.owner, uid, gid)
			}
			continue
		}
		if err != nil {
			t.Errorf("owner %q: %v", tc.owner, err)
			continue
		}
		if uid != tc.uid || gid != tc.gid {
			t.Errorf("owner %q: got %d:%d, want %d:%d", tc.owner, uid, gid, tc.uid, tc.gid)
		}
	}
}

// The ids come from the recipe, never from the build host. A recipe with no
// users: stanza cannot name an owner, however real that account is on the
// machine doing the building.
func TestResolveOwnerIgnoresTheBuildHost(t *testing.T) {
	if _, _, err := resolveOwner(&recipe.Recipe{}, "root"); err == nil {
		t.Fatal("root resolved without a users: stanza; ownership would vary by build host")
	}
}
