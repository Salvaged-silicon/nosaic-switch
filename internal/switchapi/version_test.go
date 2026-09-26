package switchapi

import (
	"os"
	"strings"
	"testing"
)

// The Broadcom datapaths are C, and answer "caps" with a contract version of
// their own, written into query.c by hand. Nothing else ties it to Version: it
// stayed at 1.7 through the 1.8 bump, and a switch reported the old contract
// while serving the new one.
func TestDatapathContractMatchesVersion(t *testing.T) {
	src, err := os.ReadFile("../../datapath/common/query.c")
	if err != nil {
		t.Skipf("no datapath source here: %v", err)
	}
	want := `\"Contract\":\"` + Version + `\"`
	if !strings.Contains(string(src), want) {
		t.Errorf("datapath/common/query.c does not report contract %s; update it with Version", Version)
	}
}
