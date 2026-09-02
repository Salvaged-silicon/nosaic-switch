package recipe

import "testing"

// A recipe whose build genuinely differs per architecture, which the OpenBCM
// SDK does: it selects a platform by a name that shares nothing with the
// architecture id, so no ${ARCH} substitution can reach it.
func TestBuildOverridesPerArch(t *testing.T) {
	b := Build{
		System:  "make",
		Targets: []string{"user_libs"},
		Env:     map[string]string{"SDK_DIR": "/staging", "KEEP": "yes"},
		After:   []string{"default-after"},
		Arch: map[string]ArchBuild{
			"powerpc": {
				Targets: []string{"platform=bmw-2_6", "user_libs"},
				Env:     map[string]string{"SDK_DIR": "/ppc"},
			},
		},
	}

	// An architecture with no entry is untouched.
	if got := b.ForArch("x86_64"); got.Targets[0] != "user_libs" || got.Env["SDK_DIR"] != "/staging" {
		t.Errorf("an arch with no override was changed: %+v", got)
	}

	ppc := b.ForArch("powerpc")
	if len(ppc.Targets) != 2 || ppc.Targets[0] != "platform=bmw-2_6" {
		t.Errorf("targets not overridden: %v", ppc.Targets)
	}
	// Env merges rather than replaces: overriding one variable must not drop
	// the others, which a whole-map replacement would do silently.
	if ppc.Env["SDK_DIR"] != "/ppc" {
		t.Errorf("env not overridden: %v", ppc.Env)
	}
	if ppc.Env["KEEP"] != "yes" {
		t.Errorf("env override dropped an unrelated variable: %v", ppc.Env)
	}
	// Fields the override does not mention are kept.
	if ppc.System != "make" || len(ppc.After) != 1 {
		t.Errorf("unmentioned fields were lost: %+v", ppc)
	}

	// And the original is not mutated, or the second architecture built in one
	// process would inherit the first one's overrides.
	if b.Env["SDK_DIR"] != "/staging" || b.Targets[0] != "user_libs" {
		t.Errorf("ForArch mutated the recipe: %+v", b)
	}
}
