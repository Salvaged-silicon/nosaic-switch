# Running NOSaic on the 7050SX2-72Q without installing it

This is how every result on this board has been produced: the image is fetched
by Aboot over HTTP and run from RAM, and **flash is never written**. The switch
reverts to whatever `boot-config` already said on the next power cycle, so a
broken image costs a reboot rather than a recovery session.

Use this for development. [install.md](install.md) is the flash route, which is
not yet proven here.

---

## What you need

- The switch's **serial console**: ttyS0, **9600** 8N1, no flow control. Aboot
  prints on the same port, so a wrong baud looks like a dead box.
- A **web server** the switch's management network can reach.
- The switch's **management address**, and a gateway.
- This board's `portmap.conf` and `polarity.conf`. **These are not in the
  repository** — see [Site configuration](#site-configuration) below.

---

## 1. Build and serve

```sh
make image BOARD=arista-7050sx2-72q PROFILE=minimal ARGS=--ram-boot
```

`--ram-boot` is the difference: the root filesystem is carried in the initramfs
rather than mounted from a disk slot, so nothing needs to exist in flash.

```sh
mkdir -p ~/serve
cp out/images/arista-7050sx2-72q/NOSaic-*-arista-7050sx2-72q.swi ~/serve/nosaic.swi
cd ~/serve && python3 -m http.server 8080
```

---

## 2. Catch Aboot

Reboot the switch and interrupt the countdown with **Ctrl-C**:

```
Press Control-C now to enter Aboot shell
^C
Aboot#
```

If the box is already running NOSaic, `reboot` from its shell is enough.

---

## 3. Give Aboot a network and boot the image

Aboot is a separate environment from EOS and comes up with nothing configured,
so an HTTP boot fails with "Network is unreachable" before it fetches anything:

```
Aboot# initnetdev
Aboot# ifconfig ma1 10.1.1.241 netmask 255.255.255.0 up
Aboot# route add default gw 10.1.1.1
Aboot# ping -c 2 10.22.1.5
Aboot# boot http://10.22.1.5:8080/nosaic.swi
```

**Wait for the link before the fetch.** The first ping after `ifconfig up`
regularly fails while a gigabit link negotiates. A script that treats that as an
error turns an expected few seconds into a failed boot — retry rather than give
up.

Configure the address at the prompt rather than setting `NETIP`/`NETGW` in
`boot-config`, which writes flash. A runtime address disappears at the next
reboot, which is exactly what you want.

The console then shows the chip coming up:

```
releasing the switch chip from reset...
  scd: reset register before: 0xffffffff
  scd: after clearing core (bit 0): 0xfffffffe
  scd: after clearing pcie (bit 1): 0xfffffffc
54 of 54 transmitters on.
  scd: pci COMMAND: 0x0000 -> 0x0002 (memory space enabled)
the switch chip is on the bus.
```

Log in as `admin`, no password.

---

## 4. Site configuration

Two files the datapath cannot start without are **deliberately not in the
repository**:

| File | What it is |
|---|---|
| `portmap.conf` | logical port to physical lane map |
| `polarity.conf` | per-lane SerDes TX/RX polarity inversions |

The numbers are the vendor's, read off the machine that already has them, so
they are yours and not ours to ship. Generate them once from a switch running
EOS:

```sh
platform/arista-7050sx2-72q/tools/mkportmap.sh  <switch-ip> > portmap.conf
platform/arista-7050sx2-72q/tools/mkpolarity.sh <switch-ip> > polarity.conf
```

They belong in `/mnt/data/config`, which on a **RAM boot is a tmpfs** — so they
have to be pushed after every boot:

```sh
doas mkdir -p /mnt/data/config
doas busybox wget -q -O /mnt/data/config/portmap.conf  http://10.22.1.5:8080/portmap.conf
doas busybox wget -q -O /mnt/data/config/polarity.conf http://10.22.1.5:8080/polarity.conf
doas s6-svc -r /run/service/nosd
```

Without them `nosd` exits and s6 restarts it in a loop; the log says which file
it wanted.

**Chip initialisation takes around six minutes** at this log verbosity. Wait
for it before concluding anything:

```sh
doas s6-svstat /run/service/nosd
grep -E "^tap:|datapath is up" /var/log/nosd.log
```

```
tap: et1 port 1 in vlan 1006, untagged
tap: et1 <-> port 1
tap: et2 port 2 in vlan 1007, untagged
tap: et2 <-> port 2
nosd: the datapath is up on unit 0
```

---

## 5. Address the ports and start routing

The taps are recreated whenever `nosd` restarts, so addresses go on afterwards.
`ip` and `ifconfig` are not on the login shell's `PATH` in the minimal image;
`doas busybox ip` works.

```sh
doas busybox ip addr add 10.101.101.42/29 dev et1
doas busybox ip addr add 10.101.101.57/29 dev et2
doas busybox ip -6 addr add 2001:db8:1040::42/64 dev et1
doas busybox ip link set et1 up
doas busybox ip link set et2 up
```

Addresses here are examples: the v4 ones are RFC 1918 and the v6 one is the
RFC 3849 documentation prefix. Substitute your own.

Then OSPF, through `vtysh`:

```sh
doas vtysh -c "conf t" \
  -c "router ospf" \
  -c "ospf router-id 10.101.101.42" \
  -c "network 10.101.101.40/29 area 0.0.0.0" \
  -c "network 10.101.101.56/29 area 0.0.0.0" \
  -c "exit" \
  -c "router ospf6" -c "ospf6 router-id 10.101.101.42" -c "exit" \
  -c "interface et1" -c "ipv6 ospf6 area 0.0.0.0" -c "exit" \
  -c "interface et2" -c "ipv6 ospf6 area 0.0.0.0" -c "end"
```

None of this persists: the overlay is a tmpfs on a RAM boot. That is the point
of this mode, and the reason flash installation is the next milestone.

---

## 6. Check it worked

```sh
doas vtysh -c "show ip ospf neighbor"
doas vtysh -c "show ipv6 ospf6 neighbor"
```

```
Neighbor ID     Pri State      Address        Interface
10.101.101.241    1 Full/DR    10.101.101.41  et1:10.101.101.42
10.101.1.241      1 Full/DR    10.101.101.58  et2:10.101.101.57
```

The routes reaching the chip, from the chip's own accounting:

```sh
grep -E "^l3: v4|^l3: v6|CHIP" /var/log/nosd.log | tail -3
```

```
l3: v4 42 routes / 2 next hops (moved 0, gone 0, failed 0, unresolved 0), 2 hosts
l3: v6 31 routes / 2 next hops (moved 5, gone 0, failed 0, unresolved 0), 2 hosts
l3: CHIP route 96/8192  intf 3/12287  host 16/16384
```

The v4 count should match the kernel's gateway routes exactly; the v6 count is
the kernel's less the default route, which is deliberately not programmed.

```sh
doas busybox ip route | grep -c via
doas busybox ip -6 route | grep -c via
```

---

## 7. Proving the chip is forwarding, not the CPU

"The routes are installed" and "the chip is forwarding" are different claims.
The second one needs traffic that transits the box and a comparison of two
counters.

Nothing routes through a switch that is a stub, so give a neighbour one
temporary reason to:

```sh
# on the neighbour -- runtime only, not written to disk
ip route add 10.101.101.58/32 via 10.101.101.42 dev swp8
ping -c 100 -i 0.05 10.101.101.58
ip route del 10.101.101.58/32 via 10.101.101.42 dev swp8
```

Take both counters either side of that:

```sh
cat /sys/class/net/et1/statistics/rx_packets          # what the CPU saw
grep -A40 "^port: et1 (port 1) link" /var/log/nosd.log \
  | grep -oE "(in|out)=[0-9]+" | tail -2               # what the chip saw
```

The chip's `in` should rise by the number of packets sent. The CPU's should
not — it should rise only by the background protocol traffic, a hello per
adjacency every ten seconds. Measured here: chip **+101**, CPU **+44**, with
four adjacencies over a four-minute window.

`nosd` prints the chip counters once a minute. The SDK's own tracing interleaves
with them, which is why the greps above use a context window rather than
matching a single line.

---

## 8. Updating just the datapath

A full image rebuild and reboot is about fifteen minutes. Replacing `nosd`
alone is about two, and the root filesystem is a writable overlay:

```sh
doas s6-svc -d /run/service/nosd
doas busybox wget -q -O /usr/sbin/nosd-td2p http://10.22.1.5:8080/nosd-td2p
doas chmod 755 /usr/sbin/nosd-td2p
doas s6-svc -u /run/service/nosd
```

Check the md5 against the build host. The chip is fully reinitialised on
restart, so this costs the same six minutes as a boot — but not the boot.

---

## Gotchas

- **`reboot` needs no `-f`.** PID 1 handles the signal, brings services down
  with a bounded wait, and calls the kernel. Earlier images had no handler and
  hung with services dead and the shell still answering.
- **Addresses are lost on every `nosd` restart**, because the tap devices are
  recreated.
- **There is a window after configuring an address** where traffic to it is
  dropped: `MY_STATION` is already routing packets aimed at us and the
  self-punt entry has not been installed yet. `l3sync` closes it within a
  second. Testing faster than that looks exactly like a broken port.
- **`vtysh` needs `doas`.** The socket directory belongs to the `frr` account.
