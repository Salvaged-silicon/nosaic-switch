# Building an image for the AS5610-52X

Nothing has been built for this board yet. This page is what the build needs,
and what of it exists — written now so the first person to try does not have
to work it out from scratch.

```sh
make toolchain ARCH=powerpc          # does not exist yet -- see below
make packages ARCH=powerpc PROFILE=minimal
make image BOARD=edgecore-as5610-52x
```

## The toolchain works — spike S1 passed

`make toolchain ARCH=powerpc` builds in about 38 minutes, and
`make toolchain-test ARCH=powerpc` passes: a statically linked 32-bit
big-endian PowerPC binary that runs, and an instruction audit reporting zero
forbidden opcodes.

The difficulty is specific and worth understanding before starting. GCC removed
the `powerpc-linux-gnuspe` target — the e500v2's native SPE ABI — around GCC
8/9. crosstool-NG will still build it, pinned to gcc-8.5.0 and glibc-2.29,
which freezes this architecture on a 2021 compiler for as long as the board
lives. So `arch/powerpc/arch.yml` chooses **soft-float generic PowerPC** on a
current toolchain instead: e500v2 executes standard PowerPC integer
instructions natively, and software floating point costs nothing measurable in
a datapath that barely uses it.

The trap that choice creates is already guarded. A hard-float build
disassembles cleanly, runs under QEMU's permissive generic-PowerPC model, and
dies with SIGILL only on the real board — so `arch.yml` carries a
`forbidden_insn_re` listing every instruction class an e500v2 cannot execute,
and the toolchain test fails a build containing one. Do not weaken it to make
something link.

That was the gate, and it is met. The concern behind it was real — GCC dropped
the SPE ABI target around GCC 8/9 — and the answer is that soft-float generic
PowerPC on a current gcc-15.2.0 and glibc-2.42 works. This hardware does not
need a frozen compiler.

## The datapath is the second

There is no `nosd-tdp`. The good news is that it is a new package rather than
a new SDK: OpenBCM 6.5.24 — the same recipe the 7050SX2 already uses — carries
this chip.

```
include/soc/devids.h:842   BCM56846_DEVICE_ID  0xb846
                           BCM56846_A0_REV_ID  1
                           BCM56846_A1_REV_ID  2      <- this board is rev 02
src/soc/esw/trident.c      the Trident+ SOC driver
src/bcm/esw/trident/       the BCM-layer driver
```

`nosd-td2p` is the closest relative and most of its shape should carry: a
userspace BDE over `soc_cm_device_vectors_t`, a tap bridge, an FIB mirror.
What must not carry is anything that assumes the 7050SX2's board — the SCD, the
port map, the flash layout, and the LED path are all different here.

This is also the first real test of the per-ASIC split. If `nosd-tdp` ends up
sharing most of its code with `nosd-td2p`, the split was drawn in the right
place. If it ends up copying it, the split is in the wrong place and the fix
belongs in the shared layer rather than in a third copy.

## Two board differences that will bite

- **The PCI domain is not zero.** The ASIC is at `0001:01:00.0`. A P2020 has
  more than one PCIe controller, and anything that hardcodes `0000:` will not
  find this chip.
- **The DMA region comes from CMA, not `memmap=`.** The vendor kernel command
  line is `console=ttyS0,115200 cma=32M`. The Trident+ needs contiguous DMA the
  same way the Trident2+ does, and the mechanism differs.

## Configuration knobs EdgeNOS needed, and which of them are ours

EdgeNOS's SDK port lists the `config.bcm` settings without which `bcm_init`
does not complete on this chip. They are recorded here because rediscovering
them costs days, and split because **not all of them apply to NOSaic**.

Likely to apply — they are about this silicon:

```
mem_clear_hw_acceleration=0    CRITICAL. Forces table clears over S-Channel.
                               DMA clears SILENTLY FAIL on this chip -- the
                               worst failure shape there is.
skip_ipmc_init=1               ipmc_init hits "Table full", and bcm_init rolls
                               back the WHOLE unit on any module error, so one
                               module failing looks like total failure.
parity_enable=0
mem_scan_enable=0
```

Probably **not** ours, and worth understanding why before copying:

```
soc_skip_reset=1        EdgeNOS ran the SDK alongside edged, which already
phy_null=1              owned the chip and the ports. NOSaic owns the chip
bcm_linkscan_interval=0 outright, as it does on the 7050SX2, so these
                        co-existence settings should not be needed -- and
                        each one switches off something a switch needs.
```

And these NOSaic already knows about from the 7050SX2, for the same reason —
no interrupt path yet:

```
polled_irq_mode=1  schan_intr_enable=0  tdma_intr_enable=0  tslam_intr_enable=0
```

That last group is worth revisiting immediately rather than inheriting. On the
7050SX2 those settings cost a core and held the control plane to about twenty
packets a second, and the fix was to make interrupts work rather than to tune
the polling. Starting this board polled is reasonable; leaving it polled is
repeating a mistake whose cost is already measured.

One parser difference to watch: EdgeNOS's `config.bcm` parser is **first-wins**
— edit in place, do not append. NOSaic's property loader is last-wins, so
files layer over each other. A knob copied from EdgeNOS notes into the wrong
position in the wrong file does nothing, and the unread-property report is what
catches it.
