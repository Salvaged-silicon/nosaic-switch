// Package version carries the build identity of the nosaic binary.
package version

// Version is the NOSaic release. Set via -ldflags at release time.
var Version = "0.0.0-dev"

// Commit is the git SHA this binary was built from. Set via -ldflags.
var Commit = "unknown"
