package check

import "testing"

// The point of hasWord is that a plain substring search reports success for
// "zip" against "bzip2" -- a false confirmation of the exact thing being
// checked, and how the aboot backend shipped without zip in the first place.
func TestHasWordDoesNotMatchInsideALongerPackageName(t *testing.T) {
	const dockerfile = "  ca-certificates cpio curl bzip2 zlib1g-dev \\\n" +
		"  u-boot-tools unzip xz-utils \\\n"

	for _, tc := range []struct {
		pkg  string
		want bool
		why  string
	}{
		{"zip", false, "matched inside bzip2 or unzip"},
		{"bzip2", true, "listed on its own"},
		{"unzip", true, "listed on its own"},
		{"u-boot-tools", true, "hyphens are part of the name"},
		{"u-boot", false, "a prefix of u-boot-tools is not the package"},
		{"zlib1g-dev", true, "digits are part of the name"},
		{"device-tree-compiler", false, "genuinely absent"},
	} {
		if got := hasWord(dockerfile, tc.pkg); got != tc.want {
			t.Errorf("hasWord(%q) = %v, want %v (%s)", tc.pkg, got, tc.want, tc.why)
		}
	}
}
