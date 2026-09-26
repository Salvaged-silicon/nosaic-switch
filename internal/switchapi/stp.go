package switchapi

import "fmt"

// ValidSTPPriority checks a bridge priority: 802.1D-2004 carries it in the top
// four bits of the bridge ID, so it is 0 to 61440 in steps of 4096. Every
// datapath refuses the same values with the same words.
func ValidSTPPriority(p int) error {
	if p < 0 || p > 61440 || p%4096 != 0 {
		return fmt.Errorf("stp priority %d: must be 0 to 61440 in steps of 4096", p)
	}
	return nil
}

// ValidSTPCost checks a port path cost: 0 for the default from the link's
// speed, or 1 to 200000000.
func ValidSTPCost(c int) error {
	if c < 0 || c > 200000000 {
		return fmt.Errorf("stp cost %d: must be 1 to 200000000, or 0 for the speed's default", c)
	}
	return nil
}

// STPDefaultCost is 802.1D-2004's recommended path cost for a link speed in
// Mb/s: 20000000 divided by the speed, so 1 Gb/s is 20000, 10 Gb/s 2000 and
// 40 Gb/s 500.
func STPDefaultCost(mbps int) int {
	if mbps <= 0 {
		return 200000000
	}
	c := 20000000 / mbps
	if c < 1 {
		c = 1
	}
	return c
}
