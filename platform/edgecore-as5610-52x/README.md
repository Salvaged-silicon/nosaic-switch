# Edgecore AS5610-52X

NOSaic's third board, and the first that is not x86_64.

**It boots to a login.** On 2026-09-02 the board ran a NOSaic image over TFTP
from the U-Boot prompt, nothing written to its disk: our own PowerPC toolchain,
our kernel, our device tree, squashfs mounted, overlay assembled, every service
started and a login on the console.

```
Linux 6.12.105 ppc
s6rc-oneshot-runner dropbear getty-console ospf6d ospfd zebra
frr-dirs frr-siteconf network nosd
```

Nothing is installed on it: the image runs from RAM and the switch returns to
its previous OS on a power cycle. See [install.md](docs/install.md) for how to
repeat it and [todo.md](docs/todo.md) for what is left.

| | |
|---|---|
| ASIC | Broadcom **BCM56846** (Trident+), `14e4:b846` at `0001:01:00.0` |
| Arch | **PowerPC e500v2**, 32-bit big-endian, dual core (Freescale P2020) |
| Memory | 2 GB |
| Front panel | 48 × SFP+ (10G) + 4 × QSFP+ (40G) — `xe0`..`xe47` and `xe48`..`xe51` |
| Boot | U-Boot + ONIE, on NOR flash |
| Console | ttyS0 @ **115200** |
| Device tree | `powerpc-accton-as5610-52x-r0` — Accton is the ODM, Edgecore the brand |
| Status | **bringup** — boots to userspace over the network; no install yet |

- **[Hardware reference](docs/hardware.md)** — what has been read off a running unit
- **[Build](docs/build.md)** — what exists and what is missing
- **[Install](docs/install.md)** — the ONIE route, and how to boot it over TFTP
- **[Configuration](docs/configuration.md)** — what this switch is set up to do,
  captured from the EdgeNOS installation it has to replace

## Why this board

It is the first port that exercises the axes independently rather than
inheriting them. The 7050SX2 and virt-x86_64 are both x86_64, so every claim
about NOSaic being architecture-neutral has been a design commitment rather
than a demonstrated fact. This board changes three things at once — a new CPU
architecture, a new ASIC family, a new bootloader — and each is meant to be a
separate, additive piece of work.

Two of the three already exist in the tree:

- **`arch/powerpc`** is fully specified, and was written with this board in
  mind: `powerpc-nosaic-linux-gnu`, soft-float, big-endian, with a
  `forbidden_insn_re` that fails a build containing any instruction an e500v2
  cannot execute. That audit exists because a hard-float build disassembles
  cleanly and runs under QEMU's permissive generic-PowerPC model, and only
  dies on the real hardware.
- **`internal/boot/onie.go` and `uboot.go`** exist as boot backends.

## What is missing

- ~~**The toolchain has never been built.**~~ Spike S1 passed: gcc-15.2.0 and
  glibc-2.42 build soft-float generic PowerPC, and the instruction audit
  reports `0 forbidden` — nothing in the output uses an opcode an e500v2
  cannot execute. This class of hardware does **not** need a pinned older
  compiler, and M8's scope is unchanged.
- **There is no Trident+ datapath.** `nosd-tdp` does not exist. The chip is a
  BCM56846, one generation before the 7050SX2's Trident2+, and `nosd-td2p` is
  the closest relative — the first real test of whether the per-ASIC split was
  drawn in the right place.
- **Nothing has been installed on it.** The unit in the lab runs EdgeNOS, and
  every fact in these pages was read off it rather than produced by NOSaic.

## Stopping nosd

Stop it through the supervisor, and pass a timeout:

    s6-rc -t 30000 -d change nosd

Without `-t`, s6 computes a deadline from an infinite relative time and
overflows this board's 32-bit `time_t`:

    s6-svlisten: fatal: unable to subscribe to events ...: Value too large
    s6-rc: warning: unable to stop service ospfd: command exited 111

Same root cause as the `s6-rc-init` overflow the image build already works
around with `-t`; it reaches anyone typing `s6-rc` by hand. A graceful stop and
start is safe -- the chip comes back to 52 ports and its links.

`kill -9` is not. It leaves the chip mid-operation, and the next attach comes
back with TXPLL lock failures, 48 of 52 ports and no link at all, while
reporting that it is serving:

    WC40 : TXPLL did not lock: u=0 p=29
    soc_reg32_read: invalid S-Channel reply, expected READ_REG_ACK
    bcm_init failed in port

Only a board reset clears it, so this is about how the chip is left rather than
the init sequence. The daemon should reset the chip on the way in and out; until
it does, do not `kill -9` a datapath daemon on this board.

## Per-port service VLANs are the resting state

`nosd` puts every front-panel port in its own VLAN, 3300+port, with the port
untagged and the CPU tagged, and empties VLAN 1. That is Cumulus's layout on
this board, by way of EdgeNOS's `vlan_init_resv_per_port()`, and it is what both
of them boot into -- so it is not a guess, it is what the two pieces of software
known to work on this hardware actually do.

The point is that a port is forwarding and fully usable while being alone in its
VLAN, so there is nothing to bridge to and no loop to form whatever the cabling
looks like. Bridging becomes something you ask for.

Emptying VLAN 1 is not tidiness. EdgeNOS's note is specific: leaving it
populated lets the chip's L2 forwarding pick the wrong egress when the CPU
injects a tagged frame on a service VID, and they watched it happen.

`tdp-probe --ports` and `--stats` instead bridge every port together in VLAN 1.
That is what demonstrates the chip moving frames between ports, and it is a loop
wherever two ports reach the same neighbour -- which this board has, with swp1
and swp2 going to the same upstream. The tool says so every time it does it.
