package switchapi

import (
	"strings"
	"testing"
)

// The text form must survive a round trip, because it is what configuration
// files hold and what the C datapaths parse for themselves: a rule that reads
// back differently from how it was written is two rules.
func TestACLRuleRoundTrip(t *testing.T) {
	cases := []struct{ in, want string }{
		{"deny", "deny"},
		{"permit in swp6 proto icmp src 10.101.101.26/32", "permit in swp6 proto icmp src 10.101.101.26/32"},
		{"deny src 10.101.101.26", "deny src 10.101.101.26/32"},
		{"deny in swp6 proto tcp dst 10.101.101.25/32 dport 22", "deny in swp6 proto tcp dst 10.101.101.25/32 dport 22"},
		{"deny ipv6 in swp6 proto icmpv6 src 2001:470:882d:1024::1/128", "deny ipv6 in swp6 proto icmpv6 src 2001:470:882d:1024::1/128"},
		{"deny in swp6 src fe80::464c:a8ff:fe31:5dab", "deny ipv6 in swp6 src fe80::464c:a8ff:fe31:5dab/128"},
		{"permit proto 89 sport 1 dport 2 proto tcp", "permit proto tcp sport 1 dport 2"},
		{"deny ip proto ospf", "deny proto ospf"},
		{"deny src 10.1.2.3/8", "deny src 10.0.0.0/8"},
	}
	for _, c := range cases {
		r, err := ParseACLRule(10, c.in)
		if err != nil {
			t.Errorf("%q: %v", c.in, err)
			continue
		}
		if got := r.String(); got != c.want {
			t.Errorf("%q -> %q, want %q", c.in, got, c.want)
		}
		back, err := ParseACLRule(10, r.String())
		if err != nil || back != r {
			t.Errorf("%q did not survive a round trip: %v %v", c.in, back, err)
		}
	}
}

// The refusals, with the words the C parser uses, so an operator gets the
// same answer on every board.
func TestACLRuleRefusals(t *testing.T) {
	cases := []struct{ in, want string }{
		{"", "empty rule"},
		{"block proto tcp", "'block': expected permit or deny"},
		{"deny in", "'in' needs a value"},
		{"deny dport 22", "sport/dport need proto tcp or udp"},
		{"deny proto udp sport 70000", "'70000' is not an L4 port"},
		{"deny proto nope", "'nope' is not an IP protocol"},
		{"deny frob 1", "'frob': not a match keyword"},
		{"deny ipv4 src 2001:db8::/32", "'2001:db8::/32' is IPv6 in an IPv4 rule"},
		{"deny ipv6 src 10.0.0.0/8", "'10.0.0.0/8' is IPv4 in an IPv6 rule"},
		{"deny src 10.0.0.0/8 dst 2001:db8::/32", "'2001:db8::/32' is IPv6 in an IPv4 rule"},
		{"deny src 300.1.1.1", "'300.1.1.1' is not an IPv4 prefix"},
		{"deny src 2001:db8::/200", "'2001:db8::/200' is not an IPv6 prefix"},
	}
	for _, c := range cases {
		_, err := ParseACLRule(10, c.in)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%q: got %v, want an error containing %q", c.in, err, c.want)
		}
	}
	if _, err := ParseACLRule(0, "deny"); err == nil {
		t.Error("sequence 0 was accepted")
	}
}

func TestACLRuleValidate(t *testing.T) {
	has := func(n string) bool { return n == "swp1" }
	r, _ := ParseACLRule(1, "deny in swp1 proto tcp dport 22")
	if err := r.Validate(has); err != nil {
		t.Errorf("a good rule was refused: %v", err)
	}
	r, _ = ParseACLRule(1, "deny in swp9")
	if err := r.Validate(has); err == nil || !strings.Contains(err.Error(), "'swp9' is not a port on this switch") {
		t.Errorf("an unknown port was not refused: %v", err)
	}
}
