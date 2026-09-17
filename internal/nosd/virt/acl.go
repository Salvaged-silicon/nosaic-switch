package virt

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os/exec"
	"sort"
	"strconv"
	"strings"

	"github.com/salvaged-silicon/nosaic-switch/internal/switchapi"
)

// Access lists on the virtual switch are nftables rules.
//
// Real ones, with the same semantics the chips give: the whole list is one
// base chain on the prerouting hook, so a rule sees a packet as it arrives on
// a port, before forwarding and before local delivery, which is where an
// ingress field processor sits. Rules are written lowest sequence number
// first; `accept` ends the chain, which is how a permit shadows a later deny;
// `drop` is gone. Every rule carries a counter, read back for ACLs.
//
// The chain is rewritten whole on every change, from one nft script, which
// the kernel applies atomically. That is better than the chips manage today
// and it is free here, so there is no reason to do worse.
//
// What decides Capabilities.ACL is whether nft works at all in this
// namespace: no binary, or no nf_tables in the kernel, and the capability is
// absent and every rule is refused as unsupported -- rather than accepted into
// a list that filters nothing.

const nftTable = "inet nosaic"
const nftChain = "acl"

// nftWorks probes for a usable nftables. It is what makes the capability an
// observation rather than a claim.
func nftWorks() bool {
	if _, err := exec.LookPath("nft"); err != nil {
		return false
	}
	return exec.Command("nft", "list", "tables").Run() == nil
}

func nftRun(script string) error {
	cmd := exec.Command("nft", "-f", "-")
	cmd.Stdin = strings.NewReader(script)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("nft: %w: %s", err, strings.TrimSpace(string(out)))
	}
	return nil
}

// nftRule is one rule in nft's words.
func nftRule(r switchapi.ACLRule) string {
	var w []string
	if r.InPort != "" {
		w = append(w, "iifname", strconv.Quote(r.InPort))
	}
	ip, next := "ip", "protocol"
	if r.Family == 6 {
		ip, next = "ip6", "nexthdr"
	}
	w = append(w, "meta", "nfproto", map[int]string{4: "ipv4", 6: "ipv6"}[r.Family])
	if r.Proto != switchapi.ACLAny {
		w = append(w, ip, next, strconv.Itoa(r.Proto))
	}
	if r.Src.IsValid() {
		w = append(w, ip, "saddr", r.Src.String())
	}
	if r.Dst.IsValid() {
		w = append(w, ip, "daddr", r.Dst.String())
	}
	l4 := map[int]string{6: "tcp", 17: "udp"}[r.Proto]
	if r.SrcPort != switchapi.ACLAny {
		w = append(w, l4, "sport", strconv.Itoa(r.SrcPort))
	}
	if r.DstPort != switchapi.ACLAny {
		w = append(w, l4, "dport", strconv.Itoa(r.DstPort))
	}
	verdict := "accept"
	if r.Action == switchapi.ACLDeny {
		verdict = "drop"
	}
	w = append(w, "counter", verdict, "comment", strconv.Quote(fmt.Sprintf("acl_%d", r.Seq)))
	return strings.Join(w, " ")
}

// apply rewrites the chain from the held rules. Caller holds the lock.
func (s *Switch) apply() error {
	seqs := make([]int, 0, len(s.acls))
	for seq := range s.acls {
		seqs = append(seqs, seq)
	}
	sort.Ints(seqs)
	var b strings.Builder
	fmt.Fprintf(&b, "table %s {\n\tchain %s {\n\t\ttype filter hook prerouting priority -300; policy accept;\n\t}\n}\n",
		nftTable, nftChain)
	fmt.Fprintf(&b, "flush chain %s %s\n", nftTable, nftChain)
	for _, seq := range seqs {
		fmt.Fprintf(&b, "add rule %s %s %s\n", nftTable, nftChain, nftRule(s.acls[seq]))
	}
	return nftRun(b.String())
}

// counters reads every rule's hit count back out of the kernel, by the
// comment each was written with.
func nftCounters() (map[int]uint64, error) {
	out, err := exec.Command("nft", "-j", "list", "chain", "inet", "nosaic", nftChain).Output()
	if err != nil {
		return nil, err
	}
	var doc struct {
		Nftables []struct {
			Rule *struct {
				Comment string            `json:"comment"`
				Expr    []json.RawMessage `json:"expr"`
			} `json:"rule"`
		} `json:"nftables"`
	}
	if err := json.Unmarshal(out, &doc); err != nil {
		return nil, err
	}
	counts := map[int]uint64{}
	for _, item := range doc.Nftables {
		if item.Rule == nil || !strings.HasPrefix(item.Rule.Comment, "acl_") {
			continue
		}
		seq, err := strconv.Atoi(strings.TrimPrefix(item.Rule.Comment, "acl_"))
		if err != nil {
			continue
		}
		for _, e := range item.Rule.Expr {
			if !bytes.Contains(e, []byte(`"counter"`)) {
				continue
			}
			var c struct {
				Counter struct {
					Packets uint64 `json:"packets"`
				} `json:"counter"`
			}
			if json.Unmarshal(e, &c) == nil {
				counts[seq] = c.Counter.Packets
			}
		}
	}
	return counts, nil
}

func (s *Switch) hasPort(name string) bool {
	for _, n := range s.names {
		if n == name {
			return true
		}
	}
	return false
}

func (s *Switch) ACLs() ([]switchapi.ACLEntry, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.nft {
		return nil, switchapi.Unsupported("acl")
	}
	counts, cerr := nftCounters()
	seqs := make([]int, 0, len(s.acls))
	for seq := range s.acls {
		seqs = append(seqs, seq)
	}
	sort.Ints(seqs)
	out := make([]switchapi.ACLEntry, 0, len(seqs))
	for _, seq := range seqs {
		r := s.acls[seq]
		e := switchapi.ACLEntry{Seq: seq, Text: r.String(), Rule: r, Parsed: true, Installed: true}
		if cerr != nil {
			e.Error = "no counter: " + cerr.Error()
		} else {
			e.Packets = counts[seq]
		}
		out = append(out, e)
	}
	return out, nil
}

func (s *Switch) SetACL(r switchapi.ACLRule) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.nft {
		return switchapi.Unsupported("acl")
	}
	if err := r.Validate(s.hasPort); err != nil {
		return err
	}
	old, had := s.acls[r.Seq]
	s.acls[r.Seq] = r
	if err := s.apply(); err != nil {
		// Leave the kernel and the list agreeing: the rule that failed is
		// not held either.
		if had {
			s.acls[r.Seq] = old
		} else {
			delete(s.acls, r.Seq)
		}
		_ = s.apply()
		return err
	}
	return nil
}

func (s *Switch) DelACL(seq int) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.nft {
		return switchapi.Unsupported("acl")
	}
	if _, ok := s.acls[seq]; !ok {
		return fmt.Errorf("no rule with sequence %d", seq)
	}
	delete(s.acls, seq)
	return s.apply()
}
