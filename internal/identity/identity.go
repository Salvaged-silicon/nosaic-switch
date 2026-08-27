// Package identity is the single definition of NOSaic's default login.
//
// It exists so that "consistent across the project" is mechanical rather than
// remembered. The base profiles that create the account, the generated
// installation documentation and the CLI all read this file; none of them
// restates it, so none of them can drift from it.
package identity

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"gopkg.in/yaml.v3"
)

// SSH describes the network login policy.
type SSH struct {
	// PasswordAuthUntilSet is false: an unconfigured switch cannot be logged
	// into over the network at all, only on the console.
	PasswordAuthUntilSet bool   `yaml:"password_auth_until_set"`
	AuthorizedKeys       string `yaml:"authorized_keys"`
}

// Identity is the default login definition.
type Identity struct {
	Account   string `yaml:"account"`
	Privilege string `yaml:"privilege"`

	SecretsDir  string `yaml:"secrets_dir"`
	ConfigFile  string `yaml:"config_file"`
	SecretsMode string `yaml:"secrets_mode"`

	PasswordHash string `yaml:"password_hash"`

	SSH SSH `yaml:"ssh"`
}

// Load reads base/identity.yml.
func Load(root string) (*Identity, error) {
	p := filepath.Join(root, "base", "identity.yml")
	b, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	var id Identity
	dec := yaml.NewDecoder(strings.NewReader(string(b)))
	dec.KnownFields(true)
	if err := dec.Decode(&id); err != nil {
		return nil, fmt.Errorf("%s: %w", p, err)
	}
	return &id, nil
}

var validPrivilege = []string{"sudo", "doas", "none"}
var validHash = []string{"yescrypt", "argon2id", "sha512"}

// Validate returns every problem with the definition.
func (i *Identity) Validate() []string {
	var errs []string
	bad := func(f string, a ...any) { errs = append(errs, fmt.Sprintf(f, a...)) }

	if i.Account == "" {
		bad("account is required")
	}
	if i.Account == "root" && i.Privilege != "none" {
		bad("account is root, so privilege should be none rather than %q", i.Privilege)
	}
	if !oneOf(i.Privilege, validPrivilege) {
		bad("privilege %q must be one of %s", i.Privilege, strings.Join(validPrivilege, ", "))
	}
	if !oneOf(i.PasswordHash, validHash) {
		bad("password_hash %q must be one of %s — a credential is stored hashed, never in plain text",
			i.PasswordHash, strings.Join(validHash, ", "))
	}

	// The rule worth enforcing rather than documenting: secrets must not live
	// in the shareable config file, because that is the file people paste into
	// bug reports and commit to repositories.
	if i.SecretsDir == "" {
		bad("secrets_dir is required")
	} else if i.ConfigFile != "" && strings.HasPrefix(i.ConfigFile, strings.TrimSuffix(i.SecretsDir, "/")+"/") {
		bad("config_file %q is inside secrets_dir %q: the shareable config must not live among secrets",
			i.ConfigFile, i.SecretsDir)
	}
	if i.SecretsMode != "0700" && i.SecretsMode != "0600" {
		bad("secrets_mode %q must be 0700 or 0600", i.SecretsMode)
	}

	// Shipping password authentication enabled before a password exists would
	// mean an unconfigured switch is reachable over the network.
	if i.SSH.PasswordAuthUntilSet {
		bad("ssh.password_auth_until_set is true: an unconfigured switch would accept " +
			"network logins before any password is set")
	}
	return errs
}

func oneOf(v string, valid []string) bool {
	for _, s := range valid {
		if v == s {
			return true
		}
	}
	return false
}
