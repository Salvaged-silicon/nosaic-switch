# How the 7050SX2-72Q works, top to bottom

What each piece does, how a packet actually travels, and why the parts that
look redundant are not. [hardware.md](hardware.md) is the register-level
reference; this is the shape of the whole thing.

Diagrams are ASCII on purpose. The console on this board is 9600 baud serial,
and a diagram you cannot read where you are debugging is not much of a diagram.

---

## 1. The board

```
                            +---------------------------+
                            |     x86_64 CPU + DRAM     |
                            +---------------------------+
                                        |
             +--------------------------+--------------------------+
             | PCIe                     | PCIe                     | PCIe
             v                          v                          v
   +-------------------+     +---------------------+    +--------------------+
   |   BCM56860        |     |   Arista SCD        |    |   BCM57762         |
   |   Trident2+       |     |   3475:0001         |    |   14e4:1682 (tg3)  |
   |   14e4:b860       |     |   at 05:00.0        |    |   at 04:00.0       |
   |   at 01:00.0      |     |                     |    |                    |
   |                   |<----|  reset, GPIO, LEDs  |    |   management port  |
   |   the switch      |     |  SMBus masters      |    |   (ma1 / eth0)     |
   +-------------------+     +---------------------+    +--------------------+
      |            |                    |
      |            |                    +--- SMBus ---> LM73, MAX6658 (temps)
      |            |                    +--- SMBus ---> Crow CPLD (4 fans)
      |            |                    +--- PMBus ---> 2 PSUs
      |            |                    +--- I2C ----> 54 transceiver cages
      |            |
      v            v
  48 x SFP+     6 x QSFP+ (24 lanes)          = 72 x 10G of front panel
```

**The SCD is the piece with no mainline equivalent.** It is a PCI-enumerated
board controller and it owns ASIC reset, so nothing about the switch chip
happens until the SCD is driven. There is no Linux driver for it; NOSaic's is
`internal/platformhal/scd`.

The ordering that follows from this is not optional:

```
   power on
      |
      v
   SCD enumerates on PCI            the ASIC does NOT appear yet:
      |                             it is held in reset
      v
   clear reset bits 0 and 1
      |
      v
   PCI rescan  ---------------->    01:00.0 appears (14e4:b860)
      |
      v
   enable PCI memory space          BAR0 is assigned but decoding is off,
      |                             so every register reads 0xffffffff
      v
   nosd attaches
```

Two traps live in that sequence, both of which report success while doing
nothing. Reset bit **1** is the PCIe block, not bit 2 — bit 2 is unimplemented
here and reads as 1 for ever, so clearing it writes to nothing and looks fine.
And a freshly rescanned device has PCI memory-space decoding **disabled**: BAR0
is assigned, reads are answered by nothing, and every register in the chip
reads `0xffffffff`.

---

## 2. The software stack

```
  +---------------------------------------------------------------+
  |  vtysh          nosaic CLI                                     |  operator
  +---------------------------------------------------------------+
           |                    |
  +----------------+   +-------------------+
  |  FRR           |   |  nosaic platform  |                          control
  |  zebra         |   |  thermal loop     |
  |  ospfd ospf6d  |   +-------------------+
  +----------------+            |
       |        |               | platform-hal
       |        | netlink       v
       |        |        +-------------+
       |        |        |  SCD driver |----> fans, PSUs, temps, LEDs, optics
       |        |        +-------------+
       |        v
       |   +-------------------------+
       |   |   Linux kernel          |
       |   |   FIB, ARP, NDP         |
       |   |   et1, et2  (tap)       |
       |   +-------------------------+
       |        ^          |
       |        |          | read()/write() on /dev/net/tun
       |        |          v
       |   +---------------------------------------------+
       +-->|  nosd  (nosd-td2p)                          |
           |                                             |
           |   tapbridge.c   frames <-> tap devices      |
           |   l3sync.c      kernel FIB -> chip tables   |
           |   sdk.c         chip bring-up               |
           |   bde.c         userspace BDE               |
           +---------------------------------------------+
                            |
                            | mmap of BAR0 + a reserved DMA pool
                            v
                    +-------------------+
                    |    BCM56860       |
                    +-------------------+
```

Everything above `nosd` is ordinary Linux. FRR is unmodified upstream and does
not know it is on a switch — it configures interfaces and writes routes into
the kernel FIB exactly as it would on a server. That is the whole point: the
switch-specific part is `nosd` reading what the kernel already decided.

### No BDE kernel module

Broadcom ships its BDE as kernel modules targeting Linux 5.10 and older; this
fleet runs 6.12, where `ioremap_nocache` alone has been gone since 5.6.
Carrying that patch set for the life of the boards is a cost with no return,
because the BDE's whole job — register access, PCI config, a DMA pool,
interrupts the SDK does not need — is doable from userspace:

```
     SDK (unmodified)  ---- soc_cm_device_vectors_t ---->  bde.c
                                                             |
                                          +------------------+------------------+
                                          |                  |                  |
                                    mmap BAR0 via      /sys/bus/pci/...    mmap the pool
                                     /dev/mem           config space       reserved by
                                                                           memmap= on the
                                                                           kernel cmdline
```

One constraint comes with it: the DMA pool must be reserved on the kernel
command line, which `board.yml` does with `memmap=64M$0xd0000000`. Everything
above that shim is the unmodified SDK, including the Trident2 MMU and LLS
initialisation that hand-reproduction repeatedly failed to match.

---

## 3. A port becomes a Linux interface

`nosd` creates one tap device per routed port from the `tap_` properties in
`config/asic.conf`:

```
    tap_et1=1:1006:1600
           |  |    |
           |  |    +--- interface MTU
           |  +-------- dedicated VLAN for this routed port
           +----------- logical port number on the chip
```

Each gets its own VLAN, its own MAC, and its own router interface. One VLAN per
port rather than one shared: these are routed interfaces, and putting two of
them in one broadcast domain would let traffic between them bypass the routing
this exists to make possible.

### Linux to the wire

```
   kernel writes an untagged frame to the tap
                    |
                    v
   +-------------------------------------------+
   | tap_tx()                                  |
   |   copy into the ONE preallocated          |
   |     bcm_pkt_alloc buffer                  |
   |   insert 802.1Q tag with the port's VLAN  |
   |   pad to 60 bytes if shorter              |
   |   tx_pbmp  = this port                    |
   |   tx_upbmp = this port  (egress untagged) |
   +-------------------------------------------+
                    |
                    v
              bcm_tx()  --> chip strips the tag --> wire, untagged
```

Three things there are load-bearing and every one of them fails silently:

- **The buffer must come from `bcm_pkt_alloc`.** `bcm_tx` hands the address
  straight to the DMA engine, which resolves it against the reserved pool. A
  stack buffer has no mapping there, so the engine reads whatever sits at the
  physical address it computes — the frame still goes out with our exact
  timing. The neighbour counted six frames for six pings and every one arrived
  as all-zero MACs and `802.3, length 0`.
- **That buffer is allocated once.** The BDE's `salloc` is a bump allocator
  with no free. Allocating per frame exhausts the 64 MB pool after ~2400
  transmits, after which nothing can transmit and the adjacencies die with
  every process still running.
- **The frame must carry a VLAN tag.** The transmit path assumes a tag at
  offset 12 and, because `tx_upbmp` marks the port untagged, strips four bytes
  there. Hand it an untagged frame and it deletes the ethertype instead:
  everything past the MAC header shifts left by four.

### The wire to Linux

```
   frame arrives, chip punts it to the CPU
                    |
                    v
   +-------------------------------------------+
   | tap_rx()  (SDK receive thread)            |
   |   match pkt->src_port to a tap            |
   |   strip the 802.1Q tag the chip added     |
   |   write() to the tap fd                   |
   +-------------------------------------------+
                    |
                    v
              the kernel sees an ordinary frame on et1
```

---

## 4. A route becomes hardware

Without this, the tap bridge alone still routes — every forwarded packet goes
up to the CPU, through the kernel, and back down. It works, and it is about
three orders of magnitude slower than the silicon it is running on.

`l3sync.c` mirrors what the kernel already decided. It decides nothing:

```
   /proc/net/route        /proc/net/arp          /proc/net/if_inet6
   /proc/net/ipv6_route   RTM_GETNEIGH           (our own addresses)
        |                  (netlink, v6)               |
        |                       |                      |
        +-----------+-----------+----------------------+
                    |
                    v
              l3sync, once a second
                    |
        +-----------+-----------+--------------------+
        v           v           v                    v
   bcm_l3_route  bcm_l3_host  bcm_l3_egress    bcm_field_entry
     (DEFIP)      (hosts)      (next hops)      (punt rules)
```

The chip objects and how they chain:

```
   bcm_l3_intf      our MAC for this VLAN -- the source MAC on routed frames
        ^
        |
   bcm_l3_egress    next hop: destination MAC + port + VLAN   <--+
        ^                                                        |
        |                                                        |
   bcm_l3_route     prefix -> egress    (routes with a gateway)  |
   bcm_l3_host      address -> egress   (directly attached) -----+

   bcm_l2_station   "frames to this MAC are mine -- route them"  (MY_STATION)
```

Two of those are easy to leave out and both produce a router that half works.

**Directly attached hosts need host entries.** Only routes with a gateway
become DEFIP entries, which leaves a hole a router cannot have: a destination
on one of our own subnets has no gateway, so nothing is programmed. That is not
an edge case — the peer at the far end of each link is exactly this, and it is
the first thing anyone pings.

**A route that moves must be replaced, not re-added.** Key the cache on
(prefix, mask, next hop) and a prefix that changed next hop looks new; the add
collides with the existing DEFIP entry, fails, and the chip goes on forwarding
to the old one. A hardware table that lags the routing table is worse than none
at all — traffic is blackholed while everything reports healthy. So the lookup
is on (prefix, mask) alone and a changed next hop is a `BCM_L3_REPLACE`. When
the second uplink came up here, 36 prefixes moved at once and all 36 were
replaced.

---

## 5. MY_STATION breaks the control plane, and how it is put back

This is the part that is not obvious. `MY_STATION` is what makes the chip route
frames addressed to our MAC — and it therefore also hands it packets aimed **at
us**, which the routing engine has no business terminating. It drops them.

The symptom is not "the control plane is down". It is an OSPF adjacency that
reaches **ExStart and stops**, because the unicast Database Description packets
are being routed instead of delivered, while hellos keep flowing.

```
   frame in
      |
      v
   +----------------------+
   | MY_STATION match?    |--- no --> switched as L2
   +----------------------+
      | yes
      v
   +----------------------------------------+
   | ingress field processor                |
   |                                        |
   |   IP protocol 89          -> CopyToCpu |   all of OSPF, v2 and v3
   |   DstIp == our address    -> CopyToCpu |   one rule per interface
   +----------------------------------------+
      |
      v
   +----------------------------------------+
   | L3 lookup                              |
   |                                        |
   |   host entry for our own address,      |
   |     BCM_L3_L2TOCPU -> deliver, unrouted|
   |   otherwise -> DEFIP -> egress -> wire |
   +----------------------------------------+
```

Three separate mechanisms, all needed. And two more station entries, because
`MY_STATION` also displaces the path multicast took:

```
   MY_STATION  01:00:5e:00:00:00 / ff:ff:ff:00:00:00    v4 multicast
   MY_STATION  33:33:00:00:00:00 / ff:ff:00:00:00:00    v6 multicast
   MY_STATION  <our MAC>         / ff:ff:ff:ff:ff:ff    per interface
```

Without the first, OSPF hellos to 224.0.0.5 stop reaching the CPU and the
adjacency stalls in the same place for an entirely different reason.

**IPv6 hides this better.** Adding `BCM_L2_STATION_IPV6` turns on v6 routing to
our MAC without the matching punt, and the thing that would normally expose it
keeps working — OSPFv3 is protocol 89, so the protocol rule punts it and the
adjacency goes Full. What breaks is neighbour discovery: an advertisement for
our own address is ICMPv6, not 89, so it is routed and dropped, and the peer's
global address sits at `FAILED` for ever while its link-local resolves fine. So
every address an interface owns is punted, link-local included.

### Field-processor counters are off by default

Attaching a statistic to a rule starts the SDK collecting it, and each
collection takes a DMA buffer from that same bump allocator. It looks healthy
for hours and then the pool is gone; after that nothing can transmit and the
adjacencies die with every process still up. `l3_fp_stats=1` turns them on for
as long as you are watching.

---

## 6. The two packet paths

This is the distinction the whole design exists to make.

```
  CONTROL PLANE -- packets for us, and protocols       MEASURED: 500 packets/s

    wire --> chip --> punt rules / self host entry --> CPU
                                                        |
                                                     tap et1
                                                        |
                                                   kernel --> FRR
                                                        |
                                                    route decision
                                                        |
                                                   kernel FIB
                                                        |
                                                     l3sync
                                                        |
                                                        v
                                                   chip tables


  DATA PLANE -- everything else                                  line rate, no CPU

    wire --> chip --> MY_STATION --> DEFIP --> egress --> wire
                                                    the CPU never sees it
```

Measured on this board: with a host route pointing 100 pings through the
switch, the chip's et1 receive counter rose by 101 while the CPU tap's receive
counter rose by 44 — exactly the background OSPF for four adjacencies over that
window, and nothing like 100.

That comparison is the test worth remembering. "The routes are installed" and
"the chip is forwarding" are different claims, and only the second one is this:

```
   chip counter    nosd logs it once a minute:  port: et1 (port 1) link=1 ...
   CPU counter     /sys/class/net/et1/statistics/rx_packets
```

### What the control plane actually carries

Measured, same test each time — 300 pings at a fixed rate from the same host
over the same port:

| rate | before | after |
|---|---|---|
| 20/s | 15% loss | **0%** |
| 50/s | 100% loss | **0%** |
| 100/s | 100% loss | **0%** |
| 200/s | 100% loss | **0%** |
| 500/s | 100% loss | **0%** |

500/s is where the test stopped, not where the path does. A bulk transfer
across it leaves the box responsive; it used to stop answering its own console
until the transfer ended.

Two things were changed and it is worth separating them, because only one moved
this number.

**The datapath takes interrupts now.** `nosaic_interrupt_connect` used to
return -1, so the SDK ran a polling thread that held a core permanently. The
chip is bound to `uio_pci_generic`, a thread blocked in `read()` on `/dev/uioN`
calls the SDK's ISR and re-arms, and `bcmPOLL` is gone — idle load average fell
from 2.07 to 0.20. This is real and worth having, and it changed the packet
rate **not at all**.

**The daemon was rescanning the FIB per packet.** `nosaic_tap_pump` calls its
tick every time `poll()` returns, and `poll()` returns the instant a packet is
waiting — so the tick ran once per packet, not once per second. It called
`nosaic_l3_poll()` unconditionally: parse `/proc/net/route`, `/proc/net/arp`
and `/proc/net/ipv6_route`, a netlink `RTM_GETNEIGH`, then walk every route and
neighbour programming the chip. That is the twenty packets a second.

The `1000` passed to the pump is a poll **timeout** — the longest it waits when
nothing is happening — and never a promise about how often the tick runs. The
statistics call in the same function was already gated by a counter, which is
what hid it: the tick looked periodic because one of the two things in it was.

The lesson is worth more than the fix. This was read as an SDK rate limit and
then as a polling-versus-interrupt problem, and it was neither. Both readings
were plausible, both had supporting evidence, and the number did not move until
somebody looked at the loop that runs per packet.

---

## 7. Boot

```
   Aboot
     |  reads /mnt/flash/boot-config  ->  SWI=flash:/NOSaic-...swi
     |                                    (or: boot http://host/nosaic.swi)
     v
   opens the SWI (a zip: members Stored, version first, BLESSED=1)
     |
     v
   runs boot0 inside it  ->  kexec our kernel + initramfs
     |
     v
   NOSaic initramfs
     |  mounts the squashfs read-only
     |  assembles the overlay
     |  moves /mnt/data across the switch_root
     v
   s6-svscan as PID 1
     |
     +-- thermal        fan curve from the board sensors
     +-- asic-release   SCD: take the chip out of reset
     +-- transceivers   turn the lasers on
     +-- nosd           chip init, taps, route sync
     +-- zebra, ospfd, ospf6d
     +-- getty on ttyS0
```

Service order is declared, not scripted: `zebra` is `after: [frr-dirs, nosd]`
and `ospfd` is `after: [zebra]`, and the same declaration renders to s6-rc
definitions here and systemd units on the full profile.

Shutdown needs saying too. `s6-svscan` is PID 1 and takes its signal handling
from scripts in the scan directory; with none there, `reboot` brings the
supervision tree down and nothing asks the kernel to restart — services dead,
shell still answering, which looks like it worked.

---

## 8. What is proven, and what is not

Proven on the switch:

| | |
|---|---|
| Chip out of reset, on the bus, initialised | yes |
| Ports link, transmit and receive | yes, on all four cabled ports |
| 40G QSFP+ cages | link at 40000 and pass traffic, cold boot, no manual step |
| Tap bridge, ARP and ICMP over hardware ports | yes |
| OSPFv2 and OSPFv3 adjacencies | Full, to two different vendors' boxes |
| Routes into the ASIC | `CHIP route 96/8192`, the chip's own count |
| Hardware forwarding, CPU not involved | measured, see section 6 |
| Addressing and OSPF across a power cycle | loopback, every routed port, daemons -- unaided |
| Fans, temperatures, PSUs, transceivers | yes |
| Graceful reboot | yes |
| Boots from its own flash, unattended | yes |

Not proven:

- **A/B slots, trial boot and rollback.** The board boots from flash, but as a
  single image Aboot loads directly. The slot machinery is CI-tested on the
  virtual platform and unexercised here.
- **The ceiling of the control plane.** 500/s at zero loss is measured; where
  it actually stops is not. Section 6 has the numbers.
- **Where site configuration lives.** Addresses and OSPF now come back after a
  power cycle, but they come back from the board's committed `config/` and
  `recipes/frr/nosaic/frr.conf`, which is the wrong home for site addressing:
  it means rebuilding an image to change an IP. `/mnt/data` is a tmpfs on a RAM
  boot, so there is still nowhere persistent for it. See [todo.md](todo.md).
- **The `full` profile.** `board.yml` says `full` (systemd); only `minimal`
  (s6) has ever been booted here.
- **ECMP.** The two uplinks have different costs, so FRR picks one. Equal costs
  would be needed to see multipath, and `l3sync` takes a single next hop per
  prefix today.
