package switchapi

import (
	"fmt"
	"net/netip"
	"strconv"
	"strings"
)

// Access lists.
//
// An ACLRule is what an operator asked for, in the terms every board shares:
// a sequence number that orders it, an address family, an action, and up to
// five things to match on. It is deliberately small. What a chip can match
// on varies enormously and the contract carries what every implementation
// can honour -- port, protocol, addresses, L4 ports -- rather than the union
// of what each one could. An implementation that cannot hold a rule refuses
// it with a reason; it never installs part of one.
//
// Rules are evaluated lowest sequence number first, the first match decides,
// and a packet no rule matches is forwarded as if there were no list. A deny
// drops the packet before forwarding and before the CPU; a permit does
// nothing to it except stop a later deny. Every rule counts what it matched.
//
// The text form is the one operators type and the one configuration files
// hold, and ParseACLRule and ACLRule.String are its two directions. The C
// datapaths parse the same grammar for themselves, so the two parsers must
// agree; the grammar is small on purpose, and docs/acl.md is its definition.

// ACLAction is what a matching rule does to the packet.
type ACLAction int

const (
	ACLPermit ACLAction = iota
	ACLDeny
)

func (a ACLAction) String() string {
	if a == ACLDeny {
		return "deny"
	}
	return "permit"
}

// ACLAny is the value of Proto, SrcPort and DstPort when the rule does not
// match on them; a zero-value Prefix means the same for Src and Dst.
const ACLAny = -1

// ACLMaxSeq bounds sequence numbers: 1 to this, inclusive.
const ACLMaxSeq = 999999

// ACLRule is one access-list rule.
type ACLRule struct {
	Seq     int
	Family  int // 4 or 6
	Action  ACLAction
	InPort  string // port name, "" for any
	Proto   int    // IP protocol or IPv6 next header, ACLAny for any
	Src     netip.Prefix
	Dst     netip.Prefix
	SrcPort int // ACLAny for any; only with TCP or UDP
	DstPort int
}

// ACLEntry is a rule as the datapath holds it: what was asked for, whether
// it made it into the hardware, and what it has matched so far.
//
// A rule can be configured and not installed -- it did not parse, names a
// port the switch does not have, or the chip refused it -- and it is listed
// anyway, with the reason, because a rule that silently vanished is the
// fault this contract exists to prevent.
type ACLEntry struct {
	Seq       int
	Text      string  // as configured
	Rule      ACLRule // meaningful when Parsed
	Parsed    bool
	Installed bool
	Packets   uint64
	Error     string
}

// ACLProtocols are the protocol names the grammar accepts, beside a number.
var ACLProtocols = map[string]int{
	"icmp": 1, "igmp": 2, "tcp": 6, "udp": 17, "gre": 47, "esp": 50,
	"ah": 51, "icmpv6": 58, "icmp6": 58, "ospf": 89, "vrrp": 112, "sctp": 132,
}

func aclProtoName(n int) string {
	for name, v := range ACLProtocols {
		if v == n && name != "icmp6" {
			return name
		}
	}
	return strconv.Itoa(n)
}

// ParseACLRule reads the text form of a rule:
//
//	<permit|deny> [ipv4|ipv6] [in <port>] [proto <name|number>]
//	              [src <prefix>] [dst <prefix>] [sport <n>] [dport <n>]
//
// A rule is IPv6 if it says so or names an IPv6 prefix, IPv4 otherwise, and
// one that says both is refused. Port existence is not checked here: that is
// the datapath's knowledge, and it refuses at SetACL.
func ParseACLRule(seq int, text string) (ACLRule, error) {
	r := ACLRule{Seq: seq, Proto: ACLAny, SrcPort: ACLAny, DstPort: ACLAny}
	if seq < 1 || seq > ACLMaxSeq {
		return r, fmt.Errorf("sequence %d is outside 1..%d", seq, ACLMaxSeq)
	}
	words := strings.Fields(text)
	if len(words) == 0 {
		return r, fmt.Errorf("empty rule")
	}
	switch words[0] {
	case "permit":
		r.Action = ACLPermit
	case "deny":
		r.Action = ACLDeny
	default:
		return r, fmt.Errorf("'%s': expected permit or deny", words[0])
	}
	family := 0
	setFamily := func(fam int, why string) error {
		if family == 0 || family == fam {
			family = fam
			return nil
		}
		return fmt.Errorf("'%s' is IPv%d in an IPv%d rule", why, fam, family)
	}
	l4 := func(s string) (int, error) {
		n, err := strconv.Atoi(s)
		if err != nil || n < 0 || n > 65535 {
			return 0, fmt.Errorf("'%s' is not an L4 port", s)
		}
		return n, nil
	}
	for i := 1; i < len(words); i++ {
		w := words[i]
		switch w {
		case "ipv4", "ip":
			if err := setFamily(4, w); err != nil {
				return r, err
			}
			continue
		case "ipv6", "ip6":
			if err := setFamily(6, w); err != nil {
				return r, err
			}
			continue
		}
		if i+1 >= len(words) {
			return r, fmt.Errorf("'%s' needs a value", w)
		}
		i++
		arg := words[i]
		var err error
		switch w {
		case "in":
			r.InPort = arg
		case "proto":
			n, ok := ACLProtocols[arg]
			if !ok {
				n, err = strconv.Atoi(arg)
				if err != nil || n < 0 || n > 255 {
					return r, fmt.Errorf("'%s' is not an IP protocol", arg)
				}
			}
			r.Proto = n
		case "src", "dst":
			fam := 4
			if strings.Contains(arg, ":") {
				fam = 6
			}
			if err := setFamily(fam, arg); err != nil {
				return r, err
			}
			p, perr := parsePrefixLoose(arg)
			if perr != nil {
				return r, fmt.Errorf("'%s' is not an IPv%d prefix", arg, fam)
			}
			if w == "src" {
				r.Src = p
			} else {
				r.Dst = p
			}
		case "sport":
			if r.SrcPort, err = l4(arg); err != nil {
				return r, err
			}
		case "dport":
			if r.DstPort, err = l4(arg); err != nil {
				return r, err
			}
		default:
			return r, fmt.Errorf("'%s': not a match keyword", w)
		}
	}
	if family == 0 {
		family = 4
	}
	r.Family = family
	if (r.SrcPort != ACLAny || r.DstPort != ACLAny) && r.Proto != 6 && r.Proto != 17 {
		// The chip reads L4 ports out of whatever follows the IP header;
		// without TCP or UDP said, that is a match on bytes of something
		// else.
		return r, fmt.Errorf("sport/dport need proto tcp or udp")
	}
	return r, nil
}

// parsePrefixLoose accepts an address without a length as a host prefix, and
// masks the address to the length the way the hardware will.
func parsePrefixLoose(s string) (netip.Prefix, error) {
	if !strings.Contains(s, "/") {
		a, err := netip.ParseAddr(s)
		if err != nil {
			return netip.Prefix{}, err
		}
		return netip.PrefixFrom(a, a.BitLen()), nil
	}
	p, err := netip.ParsePrefix(s)
	if err != nil {
		return netip.Prefix{}, err
	}
	return p.Masked(), nil
}

// String is the canonical text form: the same words in a fixed order, so two
// rules that mean the same thing read the same, and ParseACLRule(String())
// gives the rule back.
func (r ACLRule) String() string {
	var b strings.Builder
	b.WriteString(r.Action.String())
	if r.Family == 6 {
		b.WriteString(" ipv6")
	}
	if r.InPort != "" {
		b.WriteString(" in " + r.InPort)
	}
	if r.Proto != ACLAny {
		b.WriteString(" proto " + aclProtoName(r.Proto))
	}
	if r.Src.IsValid() {
		b.WriteString(" src " + r.Src.String())
	}
	if r.Dst.IsValid() {
		b.WriteString(" dst " + r.Dst.String())
	}
	if r.SrcPort != ACLAny {
		b.WriteString(" sport " + strconv.Itoa(r.SrcPort))
	}
	if r.DstPort != ACLAny {
		b.WriteString(" dport " + strconv.Itoa(r.DstPort))
	}
	return b.String()
}

// Validate is the part of checking a rule that needs the datapath: whether
// the port it names exists. Implementations call it from SetACL so that every
// one refuses the same things with the same words.
func (r ACLRule) Validate(hasPort func(string) bool) error {
	if r.Seq < 1 || r.Seq > ACLMaxSeq {
		return fmt.Errorf("sequence %d is outside 1..%d", r.Seq, ACLMaxSeq)
	}
	if r.Family != 4 && r.Family != 6 {
		return fmt.Errorf("family must be 4 or 6, not %d", r.Family)
	}
	if r.InPort != "" && hasPort != nil && !hasPort(r.InPort) {
		return fmt.Errorf("'%s' is not a port on this switch", r.InPort)
	}
	if (r.Src.IsValid() && r.Src.Addr().Is6() != (r.Family == 6)) ||
		(r.Dst.IsValid() && r.Dst.Addr().Is6() != (r.Family == 6)) {
		return fmt.Errorf("a prefix in the rule is not IPv%d", r.Family)
	}
	if (r.SrcPort != ACLAny || r.DstPort != ACLAny) && r.Proto != 6 && r.Proto != 17 {
		return fmt.Errorf("sport/dport need proto tcp or udp")
	}
	return nil
}
