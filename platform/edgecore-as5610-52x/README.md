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
- **[Install](docs/install.md)** — the ONIE route

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
