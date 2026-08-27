# recipes/ — package build recipes

One directory per package, each containing a `recipe.yml`: where the source comes
from, how to build it, what it installs, and what services it defines.

See CONTRIBUTING.md for the rules `make check` enforces — notably that every recipe
must declare a license and whether it may be published, that sources are pinned by
hash, and that recipes never hand-write init files.

The recipe engine lands at M2.
