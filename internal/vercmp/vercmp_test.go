package vercmp

import "testing"

func TestCompare(t *testing.T) {
	cases := []struct {
		a, b string
		want int
	}{
		{"1.0", "1.0", 0},
		{"1.0", "1.0.0", 0}, // a missing component is zero
		{"1.2", "1.10", -1}, // numeric, not lexical: 2 < 10
		{"10.2", "9.9", 1},  // the case string comparison gets wrong
		{"0.17", "0.9", 1},  // and again
		{"2.45", "2.44", 1},
		{"1.28.0", "1.28", 0},
		{"1.0-rc1", "1.0", -1}, // a pre-release loses to its release
		{"1.0-rc1", "1.0-rc2", -1},
		{"1.0-rc2", "1.0-rc10", -1}, // numeric inside the pre-release too
		{"1.0a", "1.0", 1},          // alphabetic sorts after numeric
		{" 1.0 ", "1.0", 0},         // surrounding space is not significant
	}
	for _, c := range cases {
		if got := Compare(c.a, c.b); got != c.want {
			t.Errorf("Compare(%q,%q) = %d, want %d", c.a, c.b, got, c.want)
		}
		if got := Compare(c.b, c.a); got != -c.want {
			t.Errorf("Compare(%q,%q) = %d, want %d (not antisymmetric)", c.b, c.a, got, -c.want)
		}
	}
}
