# Arista 7050SX2-72Q — what is left

Ordered by whether the switch works without it, not by effort. Everything here
is something established on the switch rather than suspected; where a line says
a thing does not work, it was observed doing that.

Status of the board as a whole is in [the README](../README.md); what is proven
and how it was proven is in
[architecture.md](architecture.md#8-what-is-proven-and-what-is-not).

## Required — the switch does not come up working without these

### Site configuration does not survive a reboot

**This is the only thing in this list that stops the box being a switch.**

The image installed to flash is a RAM-boot image, so the root overlay and
`/mnt/data` are tmpfs. `portmap.conf` and `polarity.conf` live in
`/mnt/data/config`, so a power cycle loses them, `nosd` exits 1, and s6
restarts it forever. Measured after the install:

```
nosd:               down (exitcode 1)
/mnt/data/config/:  empty
et1/et2:            absent
OSPF:               not enabled in vrf default
```

The switch boots, the chip comes out of reset, thermal and the HAL run — and
there is no datapath.

The fix is not a partition. Aboot resolves `flash:` by *controller*, so extra
eMMC partitions compete for `/mnt/flash` and the first one presented wins; see
[hardware.md](hardware.md#how-aboot-resolves-flash). What fits this board is
NOSaic's persistent state in **files on the existing FAT partition**, mounted by
loop — no repartitioning, nothing about Aboot's view changes, and the vendor
image stays where it is.

## Nice to have — the switch works without these

### A/B slots, trial boot and rollback

What is installed is one image Aboot boots directly. The slot machinery exists
and is CI-tested on the virtual platform; on this board it is unexercised. It
wants the same file-based layout as the item above, so the two are one piece of
work rather than two.

### The NOSaic CLI has never been run against silicon

`nosaic show ports | routes | caps`, `interface` and `route` exist and run
against the virtual datapath in CI. This board was configured with `ip` and
`vtysh`. Until the same commands work unmodified on both, "the same commands on
every switch" is a design commitment rather than a result — and that is
[M6's gate](../../../docs/MILESTONES.md), so it is the most valuable item here
after persistence.

### The MAC address is hard-coded

`config/network.conf` states `mac 44:4c:a8:eb:93:f6` because the board keeps it
in the SCD mailbox rather than anywhere `tg3` can find it. Reading the `prefdl`
would settle this and also gives board identity and `Trident0CoreVdd`. As
written, this file is correct for exactly one switch.

### The full profile has never been booted here

`board.yml` says `profile: full` (systemd). Only `minimal` (s6) has run on this
board. Either boot it or change the declaration; a board description that
disagrees with reality is worse than either.

### ECMP

`l3sync` takes a single next hop per prefix. The lab's two uplinks have
different costs so FRR picks one and this has never mattered, but equal costs
would silently use half the capacity.

### The watchdog is not armed

Every boot says so:

```
warning: the watchdog is not armed. If this wedges the box, recovery is manual.
```

Arming it means something has to pet it, and deciding what that is — and what
should happen when it fires — is the actual work.

### `nosd` restart-loops on permanently missing configuration

s6 restarts it immediately, forever, when the reason it exited will not change
by trying again. It should back off and say so once.

### Two QSFP macros are left at the global lane map

Macros 42 and 45 use `xgxs_tx_lane_map_core` rather than a per-macro exception.
Two derived exceptions were tried and refuted. Neither cage is cabled, so this
is unobserved rather than known-good.

### The platform HAL is still SCD-shaped

PSU and transceiver access is SCD-specific and the CLI reaches it through type
assertions. It works, and it is not the seam
[the design](../../../docs/DESIGN.md) describes.
