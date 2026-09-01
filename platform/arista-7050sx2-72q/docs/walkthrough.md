# From a switch in a rack to a switch that forwards

One ordered path, start to finish, for somebody holding an Arista
DCS-7050SX2-72Q-R. Every command here was run on a real one; the output shown is
what it actually printed.

The other pages are references — [build](build.md) for the build system,
[running](running.md) for the development loop, [install](install.md) for the
flash mechanism, [architecture](architecture.md) for how it works.
This is the order.

**Roughly two hours**, most of it the toolchain building unattended.

---

## What you need

| | |
|---|---|
| The switch | running EOS, with its flash intact |
| A serial console | ttyS0, **9600** 8N1, no flow control |
| A build host | Docker and make; nothing else |
| A web server | reachable from the switch's management network |
| Management addresses | one for EOS, one for Aboot to use when net-booting |

You do not need a cross-compiler, a vendor SDK licence, or anything from
Arista. The build fetches and builds what it needs.

---

## 1. Back up the flash, and check the backup

Do this first. The rest is reversible **because** the vendor's image is still on
the flash beside ours, and that is a property of the procedure rather than a
guarantee.

Aboot lives in the BIOS SPI flash, not on the eMMC, so nothing written to
`/dev/mmcblk0` can remove the bootloader — Ctrl-C at the console always reaches
an Aboot prompt. That is what makes every failure below recoverable.

From EOS:

```sh
bash ls -l /mnt/flash/            # find your EOS-*.swi
bash cat /mnt/flash/boot-config
```

Copy off, at minimum, the `EOS-*.swi` that `boot-config` names, plus
`boot-config` itself. Then check what you took, rather than trusting the
transfer:

```sh
unzip -t EOS-4.18.3.1F.swi        # a truncated SWI still looks plausible
md5sum EOS-4.18.3.1F.swi          # compare against the switch's own md5sum
```

A whole-device image is better and not much harder; see
[install.md](install.md#before-you-start).

---

## 2. Get the port map and polarity out of EOS

The datapath cannot start without these, and they are not in this repository.
Full explanation and the traps are in
[the README](../README.md#getting-the-port-map-and-polarity-from-eos); the
short version:

```sh
cd platform/arista-7050sx2-72q/tools
./mkportmap.sh  <eos-ip> > portmap.conf
./mkpolarity.sh <eos-ip> > polarity.conf
cp portmap.conf polarity.conf ../config/
```

Both are read-only against the switch. `.gitignore` keeps them untracked.

---

## 3. Build

```sh
make toolchain ARCH=x86_64                                    # ~1.5 h, once
make image BOARD=arista-7050sx2-72q PROFILE=minimal ARGS=--ram-boot
```

Use `minimal` for now: `board.yml` says `full`, but only `minimal` has been
booted on this board.

Check the line that says how much board configuration went in:

```
4 board configuration file(s) into /etc/nosaic
```

**Four is right.** Two means step 2 did not land, and the image will boot with
no datapath.

Serve the result:

```sh
cp out/images/arista-7050sx2-72q/NOSaic-*.swi ~/serve/nosaic.swi
cd ~/serve && python3 -m http.server 8080
```

---

## 4. Try it without installing anything

Boot it over the network first. Flash is untouched, so a bad image costs a
reboot rather than a recovery session.

Reboot the switch and interrupt the countdown with **Ctrl-C**, then:

```
Aboot# initnetdev
Aboot# ifconfig ma1 10.1.1.241 netmask 255.255.255.0 up
Aboot# route add default gw 10.1.1.1
Aboot# ping -c 2 <build host>
Aboot# boot http://<build host>:8080/nosaic.swi
```

The first ping after `ifconfig up` regularly fails while the link negotiates —
retry rather than concluding the network is broken.

You should see the chip come out of reset:

```
releasing the switch chip from reset...
  scd: after clearing core (bit 0): 0xfffffffe
  scd: after clearing pcie (bit 1): 0xfffffffc
54 of 54 transmitters on.
  scd: pci COMMAND: 0x0000 -> 0x0002 (memory space enabled)
the switch chip is on the bus.
```

Log in as `admin`, no password. **Chip initialisation takes about six minutes**
at this log verbosity — wait for it before concluding anything:

```sh
doas s6-svstat /run/service/nosd
doas busybox ip -o link show | grep -cE "et1|et2"
```

`up` and a non-zero count mean you have a datapath. Note that `ip` is not on the
login shell's `PATH`; `doas busybox ip` works.

---

## 5. Install it to the flash

Only once step 4 worked. Each step below proves one more thing, and nothing
before the last changes what the switch does on its own.

**Put the image on the flash** — from EOS, or from the NOSaic you just
net-booted (it can mount the vendor flash):

```sh
doas mkdir -p /mnt/vendor
doas busybox mount -t vfat /dev/mmcblk0p1 /mnt/vendor
doas busybox wget -q -O /mnt/vendor/nosaic.new http://<build host>:8080/nosaic.swi
doas busybox md5sum /mnt/vendor/nosaic.new          # compare with the build host
doas busybox mv /mnt/vendor/nosaic.new /mnt/vendor/nosaic.swi
```

Fetch to a temporary name and rename after checking, so a partial download
cannot leave an unbootable SWI.

**Dry run, from the Aboot prompt:**

```
Aboot# boot --testonly flash:/nosaic.swi
```

This unzips, stages the kernel and initrd, assembles the command line, calls
`kexec --load`, and returns to the prompt without jumping. Read the command line
it prints — the board's `memmap` reservation must be in it:

```
NOSaic: cmdline: ... memmap=64M$0xd0000000 iomem=relaxed
NOSaic: staged, not booting (testonly)
```

**Boot it for real**, `boot-config` still untouched:

```
Aboot# boot flash:/nosaic.swi
```

**Make it the default**, keeping the original as a whole file rather than a
commented-out line:

```sh
doas busybox cp /mnt/vendor/boot-config /mnt/vendor/boot-config.eos
doas sh -c "echo SWI=flash:/nosaic.swi > /mnt/vendor/boot-config"
doas busybox sync
doas busybox umount /mnt/vendor
doas /sbin/reboot
```

It now comes up on its own, with its datapath running and its ports present.

---

## 6. Address the ports and start routing

**This is configuration, not a per-boot ritual.** Put your addresses in the
board's `config/network.conf` and your OSPF in `recipes/frr/nosaic/frr.conf`,
build, and the switch comes up with all of it — loopback, every routed port,
and the routing daemons — after a power cycle, with nothing typed at the
console.

```
# platform/arista-7050sx2-72q/config/network.conf
iface lo   10.101.255.53/32
iface et1  10.101.101.42/29 mtu 1600
iface et54 10.101.101.65/29 mtu 1600
```

Two things about that file are worth understanding rather than copying.

**MTU is load bearing.** OSPF will not leave ExStart when the two ends
disagree, and an adjacency that reaches ExStart and stops is the signature.
Match your neighbours; here that is 1600.

**Keep management traffic on the management port.** Once this switch runs OSPF
it *learns* a route to your build network over the front panel, and a learned
/24 beats the default route by longest prefix. Its own management traffic then
leaves by the front panel and comes back through the CPU punt path, which
carries the switch's own management traffic — a 77 MB image at 21 KB/s instead
of 2 MB/s. Pin it:

```
route 10.22.1.0/24 via 10.1.1.1 dev eth0
```

If you would rather do it by hand for one boot, the taps are recreated whenever
`nosd` restarts, so addresses go on afterwards:

```sh
doas busybox ip addr add 10.101.101.42/29 dev et1
doas busybox ip link set et1 up
```

There is a window of about a second after configuring an address during which
traffic to it is dropped: `MY_STATION` is already routing packets aimed at us
and the self-punt entry has not been installed yet. Testing faster than that
looks exactly like a broken port.

---

## 7. Check you have a switch

An adjacency:

```sh
doas vtysh -c "show ip ospf neighbor"
```

```
Neighbor ID     Pri State      Address        Interface
10.101.101.241    1 Full/DR    10.101.101.41  et1:10.101.101.42
```

Routes in the ASIC, by the chip's own accounting rather than ours:

```sh
grep -E "^l3: v4|CHIP" /var/log/nosd.log | tail -2
```

```
l3: v4 42 routes / 2 next hops (moved 0, gone 0, failed 0, unresolved 0), 2 hosts
l3: CHIP route 96/8192  intf 3/12287  host 16/16384
```

The v4 count should match `ip route | grep -c via`.

**And that the chip is forwarding, not the CPU.** These are different claims and
only the second one is worth having. Give a neighbour one temporary reason to
route through the box, then compare two counters:

```sh
# on the neighbour, runtime only
ip route add <some-address>/32 via 10.101.101.42 dev <port>
ping -c 100 -i 0.05 <some-address>
ip route del <some-address>/32 via 10.101.101.42 dev <port>
```

```sh
# on the switch, either side of that
cat /sys/class/net/et1/statistics/rx_packets        # what the CPU saw
grep -A40 "^port: et1" /var/log/nosd.log | grep -oE "in=[0-9]+" | tail -1
```

The chip's count should rise by the number of packets sent. The CPU's should
not — only by background protocol traffic. Measured here: chip **+101**,
CPU **+44**, with four OSPF adjacencies over a four-minute window.

---

## If it goes wrong

Aboot is never modified, so the console always reaches a prompt.

| | |
|---|---|
| Bad image | Ctrl-C at Aboot → `boot flash:/EOS-4.18.3.1F.swi` |
| Wrong `boot-config` | boot by name as above, then `cp boot-config.eos boot-config` |
| Flash damaged | net-boot NOSaic (step 4), restore from your backup |
| Partition table gone | net-boot NOSaic, `dd` the device image back |
| eMMC dead | Aboot still boots; net-boot indefinitely |

## What you will not have

- **A/B slots or rollback.** One image, booted directly.
- **The `nosaic` CLI driving the datapath.** It exists and works against the
  virtual platform; on silicon this board is configured through `network.conf`,
  `frr.conf` and `vtysh`.
- **A measured ceiling on the control plane.** Packets destined for the switch
  go through the CPU, and that path is good for at least 500/s at zero loss.
  Where it actually stops has not been measured.
- **SSH on the login account.** dropbear is packaged and keys work, but they
  land on `root`: dropbear refuses an account with a blank password before it
  looks at a key, and the login account has one so the console can reach it
  without a password.

All of them are in [todo.md](todo.md), with what is required separated from
what is not.
