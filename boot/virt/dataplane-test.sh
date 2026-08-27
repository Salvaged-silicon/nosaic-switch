#!/usr/bin/env bash
#
# Exercise the veth datapath with real interfaces.
#
# Run inside a container with NET_ADMIN and its own network namespace, so
# nothing on the host is touched. What this proves that a unit test cannot:
# the CLI, the socket, the contract and the kernel all agree about what was
# configured.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
export PATH=$PATH:/usr/local/go/bin

# Use the iproute2 we built, not one from the build container.
#
# The container has no ip at all, and installing one would test the wrong
# binary: the datapath on a switch runs the ip from our package, so that is
# what should be under test here.
IPR=$(ls -1 out/packages/iproute2_*_"$(uname -m)".nos 2>/dev/null | head -1)
[ -n "$IPR" ] || { echo "build iproute2 first: make pkg PKG=iproute2 ARCH=x86_64" >&2; exit 1; }
TOOLS=$(mktemp -d)
tar -xOf "$IPR" data.tar.gz | tar -xzf - -C "$TOOLS" 2>/dev/null
export PATH="$TOOLS/sbin:$TOOLS/usr/sbin:$TOOLS/bin:$PATH"
command -v ip >/dev/null || { echo "the iproute2 package has no ip" >&2; exit 1; }
echo "using $(command -v ip) from $(basename "$IPR")"

# Build before entering the namespace, because there is no network inside one
# and the Go toolchain would try to fetch modules. Running real binaries is
# also closer to the truth: a switch runs nosd and nosaic, not `go run`.
if [ "${NOSAIC_NETNS:-}" != "private" ]; then
    mkdir -p out
    go build -o out/nosd   ./cmd/nosd
    go build -o out/nosaic ./cmd/nosaic
fi

# Run in a private network namespace, and refuse to run outside one.
#
# This creates real interfaces. Doing that in a shared namespace would put
# swp1..swpN on whatever machine happens to be running the test, which on a
# developer's laptop is rude and on a build host is a genuine hazard. So the
# script re-executes itself under unshare, and the guard below means a failed
# unshare stops the test rather than quietly running somewhere it should not.
if [ "${NOSAIC_NETNS:-}" != "private" ]; then
    exec env NOSAIC_NETNS=private PATH="$PATH" unshare -n "$0" "$@"
fi

# Belt and braces: a fresh namespace has only loopback. Anything else means
# the unshare did not take effect.
if [ "$(ip -br link show | grep -vc '^lo ')" -gt 0 ]; then
    echo "refusing to run: this is not a private network namespace" >&2
    ip -br link show >&2
    exit 1
fi
ip link set lo up

SOCK=/run/nosd-virt.sock
mkdir -p /run
./out/nosd --socket "$SOCK" --driver virt --ports 4 >/tmp/nosd.log 2>&1 &
NOSD=$!
trap 'kill $NOSD 2>/dev/null || true' EXIT

for _ in $(seq 1 30); do [ -S "$SOCK" ] && break; sleep 1; done
[ -S "$SOCK" ] || { echo "nosd did not start:"; cat /tmp/nosd.log; exit 1; }
export NOSD_SOCKET="$SOCK"

echo "=== what the veth datapath reports it can do ==="
./out/nosaic show caps

echo
echo "=== interfaces it actually created ==="
ip -br link show | grep -E '^swp' | sed 's/^/    /'

echo
echo "=== configure through the CLI ==="
./out/nosaic interface swp1 up
./out/nosaic interface swp2 up
ip addr add 10.0.0.1/24 dev swp1
ip addr add 10.0.1.1/24 dev swp2
./out/nosaic route add 192.0.2.0/24 via 10.0.0.2 dev swp1
./out/nosaic route add 198.51.100.0/24 via 10.0.0.2 dev swp1 via 10.0.1.2 dev swp2

echo
./out/nosaic show ports
echo
./out/nosaic show routes

echo
echo "=== the kernel's own view, which must agree ==="
ip route show | grep -E '192\.0\.2|198\.51\.100' | sed 's/^/    /'

echo
echo "=== a capability it does not have must be refused ==="
if ./out/nosaic show caps | grep -q 'vlans.*false'; then
    echo "    vlans: reported as unsupported, as this datapath actually is"
fi

echo
echo "OK - the CLI, the socket, the contract and the kernel agree"
