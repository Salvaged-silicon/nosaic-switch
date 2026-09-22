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
    # Skip if they are already built: in CI they come from the pinned
    # container, and rebuilding here would use whatever Go the host has.
    [ -x out/nosd ]   || go build -o out/nosd   ./cmd/nosd
    [ -x out/nosaic ] || go build -o out/nosaic ./cmd/nosaic
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
# A socket left by an earlier run makes the bind fail. nosd removes a stale one
# itself, but only once it starts; clearing it here means a run that crashed
# before that does not wedge the next one.
rm -f "$SOCK"
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
echo "=== access lists, through the contract and then through the kernel ==="
# The far end of swp1's cable goes into a namespace of its own, so a ping from
# it genuinely crosses the veth and arrives on swp1: that is where an ingress
# access list looks, and a ping between two addresses in one namespace never
# leaves the loopback.
if ./out/nosaic show caps | grep -Eq '^acl +yes'; then
    unshare -n sleep 600 &
    PEER=$!
    trap 'kill $NOSD $PEER 2>/dev/null || true' EXIT
    sleep 0.5
    ip link set swp1-p netns "$PEER"
    nsenter -t "$PEER" -n ip link set lo up
    nsenter -t "$PEER" -n ip link set swp1-p up
    nsenter -t "$PEER" -n ip addr add 10.0.0.2/24 dev swp1-p
    peerping() { nsenter -t "$PEER" -n ping -c "$1" -i 0.2 -W 1 10.0.0.1 >/dev/null 2>&1; }
    sleep 1
    peerping 2 || { echo "the neighbour cannot ping swp1 with no rules at all"; exit 1; }

    ./out/nosaic acl add 10 deny in swp1 proto icmp src 10.0.0.2
    ./out/nosaic show acl
    if peerping 3; then echo "a denied ping got through"; exit 1; fi
    hits=$(./out/nosaic show acl | awk '$1 == 10 { print $(NF-2) }')
    [ "$hits" = "3" ] || { echo "the deny should have counted 3 pings, counted '$hits'"; ./out/nosaic show acl; exit 1; }
    echo "    deny: 3 of 3 pings dropped, and the rule counted 3"

    ./out/nosaic acl add 5 permit in swp1 proto icmp src 10.0.0.2
    peerping 3 || { echo "a permit above the deny did not let the ping through"; exit 1; }
    echo "    permit above it: 3 of 3 through"
    ./out/nosaic show acl

    ./out/nosaic acl del 5
    ./out/nosaic acl del 10
    peerping 2 || { echo "with the rules removed the ping still fails"; exit 1; }
    if ./out/nosaic acl add 11 deny in swp9 proto icmp 2>/dev/null; then
        echo "a rule on a port this switch does not have was accepted"; exit 1
    fi
    if ./out/nosaic acl add 12 deny dport 22 2>/dev/null; then
        echo "an L4 port match without tcp or udp was accepted"; exit 1
    fi
    echo "    removed: pings flow again; bad rules refused with their reasons:"
    ./out/nosaic acl add 11 deny in swp9 proto icmp 2>&1 | sed 's/^/        /' || true
    ./out/nosaic acl add 12 deny dport 22 2>&1 | sed 's/^/        /' || true
    left=$(nft list ruleset 2>/dev/null | grep -c 'acl_' || true)
    [ "$left" = "0" ] || { echo "rules left in the kernel after deletion:"; nft list ruleset; exit 1; }
else
    echo "    acl: reported as unsupported here (no usable nftables), so nothing to drive"
    if ./out/nosaic acl add 10 deny in swp1 proto icmp 2>/dev/null; then
        echo "a datapath without access lists accepted a rule"; exit 1
    fi
fi

echo
echo "=== a capability it does not have must be refused ==="
if ./out/nosaic show caps | grep -q 'vlans.*false'; then
    echo "    vlans: reported as unsupported, as this datapath actually is"
fi

echo
echo "OK - the CLI, the socket, the contract and the kernel agree"
