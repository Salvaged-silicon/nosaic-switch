# Arista 7050SX2-72Q — what is left

Ordered by whether the switch works without it, not by effort. Everything here
is something established on the switch rather than suspected; where a line says
a thing does not work, it was observed doing that.

Status of the board as a whole is in [the README](../README.md); what is proven
and how it was proven is in
[architecture.md](architecture.md#8-what-is-proven-and-what-is-not).

## Required — the switch does not come up working without these

### The control plane's ceiling is unmeasured

**This was the top item on this list and is largely solved.** It is kept
because the number above which it fails is still unknown, and because how it
was found is worth more than the fix.

Measured, 300 pings at a fixed rate from the same host over the same port:

| rate | before | after |
|---|---|---|
| 20/s | 15% loss | **0%** |
| 50/s | 100% loss | **0%** |
| 100/s | 100% loss | **0%** |
| 200/s | 100% loss | **0%** |
| 500/s | 100% loss | **0%** |

500/s is where the test stopped. A bulk transfer over the same path leaves the
box responsive; it used to take the datapath down until the transfer ended.

What remains: nobody has found the rate at which this path actually saturates,
or what happens at it. A switch should degrade rather than fall over, and that
has not been demonstrated. Somebody should push it until it breaks and write
down what breaking looks like.

Two changes got here, and separating them matters:

  - **interrupts instead of polling.** `nosaic_interrupt_connect` returned -1,
    so the SDK ran a polling thread holding a core permanently. The chip is now
    bound to `uio_pci_generic`, a thread blocked in `read()` calls the SDK's
    ISR and re-arms, `bcmPOLL` is gone, and idle load fell from 2.07 to 0.20.
    It changed the packet rate **not at all**.
  - **the daemon stopped rescanning the FIB per packet.** `nosaic_tap_pump`
    calls its tick every time `poll()` returns, and `poll()` returns the
    instant a packet is waiting, so the tick ran once per packet.
    `datapath_tick` called `nosaic_l3_poll()` unconditionally — parse three
    `/proc` files, a netlink `RTM_GETNEIGH`, then walk every route and
    neighbour programming the chip. **That** was the twenty packets a second.

The `1000` passed to the pump is a poll timeout, not a promise about how often
the tick runs. The statistics call beside it was already gated by a counter,
which is exactly what hid the problem: the tick looked periodic because one of
the two things in it was.

It was read as an SDK rate limit, then as a polling-versus-interrupt problem.
Both readings were plausible and both had supporting evidence. The number did
not move until somebody read the loop that runs once per packet.

### Management traffic follows learned routes

This switch runs OSPF on its front panel, so it **learns** a route to the build
network over it, and a learned /24 beats the default route by longest prefix.
Its own management traffic then leaves by the front panel and returns through
the punt path above. Pulling an image ran at **21 KB/s** and saturated the box;
pinned to the management port the same transfer runs at **over 2 MB/s**.

Nothing is misconfigured. It is what happens when a router with a management
port also participates in the routing domain that contains its management
station: in-band and out-of-band share one table, and the more specific route
wins whichever one you meant.

The board ships a static route as a pin, which works because a static route's
metric beats OSPF's at equal prefix length. It is a pin and not a solution: it
names one network, and any other prefix the switch learns can do the same
again. The real answer is a **management VRF** — eth0 and its routes in a
separate table, which is also what an operator expects on a switch.

### There is no way in over the network

NOSaic packages no SSH server, so the only shell is the serial console at 9600
baud. Every remote operation costs minutes, and recovering a switch means
having someone at a console. This is a missing package rather than a missing
capability — dropbear with the board's `authorized_keys` in `config/` would do
it — but it shapes every other piece of work on this board.

### Site configuration lives in the image

Addresses and OSPF now survive a power cycle, and they do it by shipping in the
image: the board's `config/network.conf` and `recipes/frr/nosaic/frr.conf`.
That means changing an IP means rebuilding an image, which is the wrong shape.
On a RAM boot `/mnt/data` is a tmpfs, so there is still nowhere persistent for
it. This wants either a site-config input to the image build or NOSaic's
persistent state in files on the flash partition; see
[hardware.md](hardware.md#how-aboot-resolves-flash) for why it is files rather
than a partition.

## Nice to have — the switch works without these

### A/B slots, trial boot and rollback

What is installed is one image Aboot boots directly. The slot machinery exists
and is CI-tested on the virtual platform; on this board it is unexercised. It
wants the same file-on-the-flash-partition layout the item above does, so the
two are one piece of work rather than two.

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

## Build and tooling

### `make image` does not rebuild changed C source

`make image` composes the image from the prebuilt `.nos` packages under
`out/packages/`. It does not notice that a recipe's source has changed, so
editing `datapath/td2p/*.c` and running `make image` produces an image
containing the *previous* binary, silently and with no warning.

The workaround is `make pkg PKG=nosd-td2p ARCH=x86_64` before `make image`.
The fix is for the image build to compare recipe source mtimes (or hashes)
against the package it is about to use, and either rebuild or refuse.

This is worth fixing ahead of most things on this list because of how it
fails. An image that silently carries stale code does not look broken --
it boots, it runs, and it disagrees with the source tree. Several
conclusions in the 40G bring-up were drawn from images that did not
contain the change being tested, including a "the SCD fix works" that had
to be withdrawn and re-proven.

Until it is fixed, verify before booting:

    unsquashfs -d r -f nosaic-rootfs.sqsh usr/sbin/nosd-td2p
    grep -c '<a string from your change>' r/usr/sbin/nosd-td2p
