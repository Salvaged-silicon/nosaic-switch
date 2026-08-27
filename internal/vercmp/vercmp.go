// Package vercmp compares package version strings.
//
// NOSaic versions are whatever upstream calls its release: "0.17", "10.2",
// "1.28.0", "2.45", "6.16.0-rc1". They are not required to be semver, because
// the software being packaged is not. So this compares dotted numeric
// components leftmost-first, falls back to string comparison for components
// that are not numeric, and treats a missing component as zero — which makes
// "1.2" and "1.2.0" equal, as everyone expects.
//
// A pre-release suffix (anything after '-') sorts *before* the release it
// qualifies, so 1.0-rc1 < 1.0. That is the semver rule and the intuitive one.
package vercmp

import (
	"strconv"
	"strings"
)

// Compare returns -1 if a < b, 0 if equal, +1 if a > b.
func Compare(a, b string) int {
	aCore, aPre := split(a)
	bCore, bPre := split(b)

	if c := compareParts(aCore, bCore); c != 0 {
		return c
	}

	// Equal cores: a pre-release loses to a plain release.
	switch {
	case aPre == "" && bPre == "":
		return 0
	case aPre == "":
		return 1
	case bPre == "":
		return -1
	}
	return compareParts(strings.Split(aPre, "."), strings.Split(bPre, "."))
}

func split(v string) ([]string, string) {
	core, pre, found := strings.Cut(strings.TrimSpace(v), "-")
	if !found {
		pre = ""
	}
	return strings.Split(core, "."), pre
}

func compareParts(a, b []string) int {
	n := max(len(a), len(b))
	for i := range n {
		// A missing component is zero: 1.2 == 1.2.0.
		x, y := "0", "0"
		if i < len(a) {
			x = a[i]
		}
		if i < len(b) {
			y = b[i]
		}
		if c := comparePart(x, y); c != 0 {
			return c
		}
	}
	return 0
}

func comparePart(x, y string) int {
	xi, xErr := strconv.Atoi(x)
	yi, yErr := strconv.Atoi(y)

	switch {
	case xErr == nil && yErr == nil:
		// Numeric compare, so 10 > 9 rather than "10" < "9".
		if xi != yi {
			if xi < yi {
				return -1
			}
			return 1
		}
		return 0
	case xErr == nil:
		// Numeric sorts before alphabetic: 1.0 < 1.0a
		return -1
	case yErr == nil:
		return 1
	default:
		// Neither is purely numeric, but either may contain digits:
		// "rc2" must sort before "rc10", which a plain string compare
		// gets backwards.
		return naturalCompare(x, y)
	}
}

// naturalCompare compares two strings chunk by chunk, treating runs of digits
// as numbers. Digits sort before letters, so "1a" < "a1".
func naturalCompare(x, y string) int {
	for x != "" && y != "" {
		xDigit, yDigit := isDigit(x[0]), isDigit(y[0])
		if xDigit != yDigit {
			if xDigit {
				return -1
			}
			return 1
		}

		var xc, yc string
		xc, x = takeRun(x, xDigit)
		yc, y = takeRun(y, yDigit)

		if xDigit {
			if c := compareNumeric(xc, yc); c != 0 {
				return c
			}
			continue
		}
		if c := strings.Compare(xc, yc); c != 0 {
			return c
		}
	}
	switch {
	case x == "" && y == "":
		return 0
	case x == "":
		return -1
	default:
		return 1
	}
}

// compareNumeric compares digit strings without converting them, so an
// absurdly long version component cannot overflow.
func compareNumeric(x, y string) int {
	x = strings.TrimLeft(x, "0")
	y = strings.TrimLeft(y, "0")
	if len(x) != len(y) {
		if len(x) < len(y) {
			return -1
		}
		return 1
	}
	return strings.Compare(x, y)
}

func takeRun(s string, digits bool) (run, rest string) {
	i := 0
	for i < len(s) && isDigit(s[i]) == digits {
		i++
	}
	return s[:i], s[i:]
}

func isDigit(b byte) bool { return b >= '0' && b <= '9' }
