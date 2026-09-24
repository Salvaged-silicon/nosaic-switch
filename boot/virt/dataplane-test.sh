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

# And ping from our busybox, when the container has none -- it has none, and a
# ping that cannot run fails exactly like a ping that got no answer. The VLAN
# checks below are all pings, so without this they failed on a bridge that was
# forwarding correctly.
if ! command -v ping >/dev/null; then
    BB=$(ls -1 out/packages/busybox_*_"$(uname -m)".nos 2>/dev/null | head -1)
    [ -n "$BB" ] || { echo "no ping here: build busybox first: make pkg PKG=busybox ARCH=x86_64" >&2; exit 1; }
    # Its own directory: busybox links ip too, and that must not shadow
    # iproute2's.
    BBDIR=$(mktemp -d)
    tar -xOf "$BB" data.tar.gz | tar -xzf - -C "$BBDIR" 2>/dev/null
    mkdir -p "$TOOLS/bin"
    ln -sf "$BBDIR/bin/busybox" "$TOOLS/bin/ping"
    echo "using ping from $(basename "$BB")"
fi

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
./out/nosd --socket "$SOCK" --driver virt --ports 6 >/tmp/nosd.log 2>&1 &
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
echo "=== vlans and routed vlan interfaces, with traffic ==="
if ./out/nosaic show caps | grep -Eq '^vlans +true'; then
    # The contract suite first, over the socket, against the real bridge: the
    # same checks the reference implementation passes.
    ./out/nosaic verify contract | sed 's/^/    /'

    # Three hosts, each in a namespace of its own on the far end of a port.
    #   A  swp3  access 10   10.10.0.2, untagged
    #   B  swp4  trunk 10,20 10.10.0.3 on a .10 subinterface, so tagged
    #   C  swp5  access 20   10.10.0.4 -- the same subnet in another VLAN
    host() {
        unshare -n sleep 600 & local pid=$!
        HOSTS="$HOSTS $pid"
        trap 'kill $NOSD ${PEER:-} $HOSTS 2>/dev/null || true' EXIT
        sleep 0.3
        ip link set "$1" netns "$pid"
        nsenter -t "$pid" -n ip link set lo up
        nsenter -t "$pid" -n ip link set "$1" up
        eval "$2=$pid"
    }
    HOSTS=""
    host swp3-p A
    host swp4-p B
    host swp5-p C
    nsenter -t "$A" -n ip addr add 10.10.0.2/24 dev swp3-p
    nsenter -t "$B" -n ip link add link swp4-p name swp4-p.10 type vlan id 10
    nsenter -t "$B" -n ip link set swp4-p.10 up
    nsenter -t "$B" -n ip addr add 10.10.0.3/24 dev swp4-p.10
    nsenter -t "$C" -n ip addr add 10.10.0.4/24 dev swp5-p

    ./out/nosaic vlan add 10
    ./out/nosaic vlan add 20
    ./out/nosaic switchport swp3 access 10
    ./out/nosaic switchport swp4 trunk 10,20
    ./out/nosaic switchport swp5 access 20
    for p in swp3 swp4 swp5; do ./out/nosaic interface "$p" up; done
    ./out/nosaic show vlans | sed 's/^/    /'
    sleep 1

    pingfrom() { nsenter -t "$1" -n ping -c 2 -i 0.2 -W 1 "$2" >/dev/null 2>&1; }
    # What the bridge holds, for when a ping says only that it failed.
    bridgestate() {
        echo "--- bridge vlan show"; bridge vlan show
        echo "--- bridge fdb show br br0"; bridge fdb show br br0
        echo "--- ip -d link show master br0"; ip -br link show master br0
        ip -d link show br0 | head -4
        echo "--- A"; nsenter -t "$A" -n ip -br addr; nsenter -t "$A" -n ip neigh
        echo "--- B"; nsenter -t "$B" -n ip -br addr; nsenter -t "$B" -n ip neigh
    }
    pingfrom "$A" 10.10.0.3 || { echo "access 10 cannot reach the trunk's tagged 10"; bridgestate; exit 1; }
    echo "    A (access 10, untagged) -> B (trunk, tagged 10): switched"
    if pingfrom "$A" 10.10.0.4; then echo "vlan 10 reached vlan 20: no isolation"; exit 1; fi
    echo "    A (vlan 10) -> C (vlan 20, same subnet): refused, as it must be"

    # A routed interface for vlan 10. Addresses on it are ordinary addresses.
    ./out/nosaic svi add 10 | sed 's/^/    svi: /'
    ip addr add 10.10.0.1/24 dev vlan10
    pingfrom "$A" 10.10.0.1 || { echo "the access host cannot reach vlan10"; exit 1; }
    pingfrom "$B" 10.10.0.1 || { echo "the tagged host cannot reach vlan10"; exit 1; }
    echo "    vlan10 answers both the untagged and the tagged member"
    if pingfrom "$C" 10.10.0.1; then echo "vlan 20's host reached vlan 10's svi"; exit 1; fi
    ./out/nosaic show vlans | sed 's/^/    /'

    if ./out/nosaic vlan del 10 2>/dev/null; then
        echo "vlan 10 was deleted with its svi still on it"; exit 1
    fi
    ./out/nosaic svi del 10
    for p in swp3 swp4 swp5; do ./out/nosaic switchport "$p" none; done
    ./out/nosaic vlan del 10
    ./out/nosaic vlan del 20
    if ip -o link show swp3 | grep -q "master"; then
        echo "swp3 left every vlan and is still bridged"; exit 1
    fi
    echo "    removed: svi, memberships and vlans; the ports route again"
else
    echo "    vlans: reported as unsupported here (no bridge tool), so nothing to drive"
    if ./out/nosaic vlan add 10 2>/dev/null; then
        echo "a datapath without vlans accepted one"; exit 1
    fi
fi

echo
echo "OK - the CLI, the socket, the contract and the kernel agree"
