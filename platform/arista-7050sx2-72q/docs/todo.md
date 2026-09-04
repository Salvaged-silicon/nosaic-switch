# Arista 7050SX2-72Q — what is left

Ordered by whether the switch works without it, not by effort. Everything here
is something established on the switch rather than suspected; where a line says
a thing does not work, it was observed doing that.

Status of the board as a whole is in [the README](../README.md); what is proven
and how it was proven is in
[architecture.md](architecture.md#8-what-is-proven-and-what-is-not).

## Fixed on 2026-09-03

- **The SDK log was filling the box.** `nosd-td2p` passed every SDK message to
  its log at 176 KB/s, and this board RAM-boots, so the log had taken all 1.9 GB
  of `/mnt/data`: writes to `/etc` returned I/O errors and punt latency was
  718 ms. The severity filter `nosd-tdp` already had now applies here too --
  measured after: 0 bytes of growth in 30 seconds, filesystem at 0%, and 1.9 ms
  from the AS5610 where it had been 21.9 ms.
- **Periodic work ran on the packet thread.** `nosaic_tap_pump` calls its tick on
  every poll wakeup rather than on a timer. The guards here kept the *rate*
  right; they did not move it off the thread that moves packets, so every FIB
  mirror stopped forwarding for as long as it took. Now on its own thread.
- **The chassis status lamps were lying.** Four healthy fans and two fitted
  supplies, and the panel showed fan, PSU1 and PSU2 all **red** -- the state the
  CPLD powered up in, which nothing had ever corrected. `internal/platformhal/scd/statusled.go`
  now renders them from the thermal loop's own measurements, so there is no
  second notion of health that could drift from what the cooling acts on. The
  blue beacon is deliberately excluded: it shares a lamp with the status light
  and wins, and a health loop must not switch it off under an operator who lit
  it to find the box. `nosaic platform beacon on|off` sets it, `platform status`
  reports every lamp, and stopping the loop turns status **amber** rather than
  leaving a stale green claim. Verified on the running unit: red -> green,
  amber when the loop stops, green again when it comes back.

  The register offsets are **not in the tree**. They are vendor
  board-description data -- unlike the SCD offsets and the fan registers, these
  five are in nothing the vendor publishes -- so `tools/mkstatusleds.sh` ships
  and its output does not, the same arrangement as `mkportmap.sh` and
  `mkpolarity.sh`. Generate `config/statusleds.conf` once against your own
  switch. Without it the lamps report as unconfigured and are never written,
  which is the right failure: a panel driven from a guessed map is a switch
  telling an operator something false about its own health.
- **`board.yml` declared the wrong profile.** It said `full` (systemd) while
  every working image on it was `minimal` (s6). Installing the declared default
  produced a box that reached "Started ospf6d" and never came up, and cost an
  OSPF adjacency. Corrected -- and worth remembering that the build was perfectly
  happy, so the failure landed on hardware.

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

### SSH lands on root, not on the login account

Solved enough to stop being a blocker: dropbear is packaged, keys come from the
board's gitignored `config/authorized_keys`, and login takes 0.096 s against
minutes for the console.

What is left is which account it reaches. dropbear refuses any account whose
password field is **blank** before it looks at a key, and the login account has
a blank password so the serial console can reach it without one. Root's is
locked -- not blank -- so root is the only account a key can authenticate.

Fixing it properly means locking the login account and giving the console an
automatic login. That works and was tried; it removes the login prompt, which
the image boot test waits for and which is the one thing a person expects to
see on a console. Worth doing deliberately, with the test updated in the same
change, rather than as a side effect of packaging an SSH server.

### Site configuration on flash, and what it does not cover

Solved for the things that matter day to day, and worth knowing the shape of.

A switch's own settings go in `nosaic/config/` **on the flash partition**,
which survives both a reboot and the image being replaced:

```
/mnt/flash/nosaic/config/network.conf     addresses and routes
/mnt/flash/nosaic/config/frr.conf         routing configuration
```

The initramfs copies that directory into `/mnt/data/config` before the root
filesystem is assembled, and from there the existing mechanism takes over --
`apply-network.sh` prefers `/mnt/data/config/network.conf` over the image's,
and a `frr-siteconf` oneshot installs `frr.conf` with the ownership the daemons
need before zebra starts. `nosd` already read its port map and polarity from
that directory, so this is the same path rather than a new one.

Proven on the switch: an address present only in the flash copy appeared on the
interface after a cold boot.

What is left:

  - **Nothing writes it.** Configuration is edited by hand on the flash
    partition. `nosaic` has no command that changes a setting and stores it,
    which is what an operator would expect and what the CLI gate at M6 is
    really about.
  - **The partition is found by looking.** The initramfs tries a short list of
    devices and takes the first with a `nosaic/config` directory on it. That is
    honest on a board with one flash device and wants stating properly once a
    second board has two.
  - **No validation.** A malformed `network.conf` is reported line by line and
    otherwise ignored; a malformed `frr.conf` is the routing daemons' problem
    and they are less forgiving.

## Nice to have — the switch works without these

### The MAC address is hard-coded

`config/network.conf` states `mac 44:4c:a8:eb:93:f6` because the board keeps it
in the SCD mailbox rather than anywhere `tg3` can find it. Reading the `prefdl`
would settle this and also gives board identity and `Trident0CoreVdd`. As
written, this file is correct for exactly one switch.

### The full profile has never been booted here

`board.yml` says `profile: full` (systemd). Only `minimal` (s6) has run on this
board. Either boot it or change the declaration; a board description that
disagrees with reality is worse than either.

### ~~ECMP~~ — fixed in the shared datapath, untested here

`l3sync` read routes from `/proc/net/route`, which lists one gateway per prefix,
so a multipath route was programmed as a single path and reported success. It
now reads them over netlink and builds ECMP groups, and the multipath hash is
configured -- without that the group exists and sends everything down one
member anyway.

Proven on the AS5610: 150 transit packets across 30 destinations, 80 and 70
across the pair. **Not exercised here**, because this box's two uplinks have
different costs, so nothing offers it an equal-cost route to try. Give it one
before trusting it.

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

## Features — what this board could do and does not yet

Shared with the AS5610 where marked *(shared)*: both run `datapath/common`, so
one fix lands on both. The AS5610's list is
[here](../../edgecore-as5610-52x/docs/todo.md#features--what-this-board-could-do-and-does-not-yet).

### Forwarding

- **ACLs and CoPP.** *(shared)* This board's punt path is built on
  field-processor rules, so it has FP working where the AS5610 does not -- which
  makes it the right box to build the ACL model on, and the AS5610 the one that
  proves it portable.
- **VLANs as a user-facing feature.** *(shared)* No way to say "these ports are
  VLAN 100, tagged on the uplink". The datapath has the calls.
- **Link aggregation.** *(shared)* No LACP, no static bonds, on a box with six
  40G uplinks where it matters more than on the AS5610.
- **Storm control and policers.** *(shared)* Nothing rate-limits flooding.
- **VXLAN**, which the silicon and the SDK both already have — see its own
  section below.

### Control plane

- **BGP** and **BFD.** *(shared)* FRR carries both; neither has run here.

### The box itself

- **The platform HAL is SCD-shaped** — see its own section. This board *can*
  run the Go CLI, unlike the AS5610, so it is where the HAL stays honest.
- **~~Chassis status lamps~~** — done, see *Fixed on 2026-09-03*.
- **Port LED blink is unused.** *(shared)* Bit 24 flashes and nothing drives it.
  The obvious meaning is traffic, which costs a per-port counter sweep every
  interval — the same shape as the collection that exhausted the DMA pool here.
  Worth doing only alongside a counter cache something else already maintains.
- **The watchdog is not armed** — its own section.
- **Two QSFP macros are left at the global lane map** — its own section.

### ~~Configuration does not persist here~~ — it does, via the bootloader's flash

`/mnt/data` is a **tmpfs** on this board, because the SWI is a RAM-boot image:
the root filesystem travels inside the initramfs and no partition is mounted for
data. A setting written there is correct until the next reboot and then gone,
which is the worst kind of wrong for configuration.

It persists anyway, because the bootloader's own filesystem does. The initramfs
already copied `nosaic/config/` off that partition into `/mnt/data/config` at
boot -- that half was built with the board -- and `nosaic config set` now writes
back through to it: flash for durability, and the tmpfs as well so `config show`
reflects the change immediately rather than only after a reboot. `unset` writes
through the same way.

Verified on the hardware: a setting written, found in
`flash:/nosaic/config/local.conf`, and **still present after a reboot**.

The flash partition is mounted read-write for exactly as long as one file takes
to write, and only ever written inside its own subdirectory -- the boot images
live on that partition too.

**This board now has the rest of it too.** The slots and the data partition are
files on the bootloader's own filesystem, loop-mounted -- there was never room
for partitions, and Aboot resolving `flash:` by controller means adding them
could have hijacked its own boot device. So the board has A/B slots, a
persistent per-slot overlay, trial boots and rollback, and configuration lives
in the ext4 data image rather than being written through to flash a file at a
time. The write-through path above is kept because it is what makes a net-booted
board's configuration durable, and net-boot is still the development loop.

### ~~The NOSaic CLI has never been run against silicon~~ — M6's gate is met

`nosaic show ports` and `nosaic show caps` now run against Trident2+ and return
the chip's own answers -- `driver td2p`, and et1/et2 at 10000 with et53/et54 at
40000, read back from the hardware rather than from configuration. The same
commands, unmodified, that the CI datapath test runs against veth.

That is the M6 gate, and it was the part of it still open: the board booted,
the HAL reported real sensors and traffic forwarded months ago, but the daemon
did not serve the socket, so the claim that the abstraction survives contact
with real silicon had never actually been tested. The comment at the top of
`datapath/td2p/main.c` still said the daemon "does not yet attach the device or
serve the socket. Nothing here has run on hardware" while the board was running
it -- which is the kind of stale comment nobody re-checks, because a note saying
something does not work yet is not something anyone doubts.

Two things came out of doing it. The protocol documented itself as "one request
and one response per connection" and the CLI does not work that way -- it dials
once and sends every call down the same socket -- so a server that answers one
and closes gets the first call right and breaks the second with a write error
that reads as a network fault. The comment is corrected and the server
multiplexes.

And `show ports` briefly meant two different things on the two boards. It is a
contract command, so it means the ports as the datapath reports them, on every
switch; the Linux-against-the-chip comparison is a different question and now
has its own verb, `verify`. A command that means one thing here and another
there would be found by an operator moving between two switches, which is the
worst possible place to find it.

### Operating it

- ~~**A/B slots, trial boot and rollback**~~ — done, and proven on this board
  in both directions: a healthy image commits itself, and one that boots and
  does not forward rolls back unattended after three attempts.
- **Counters an operator can see.** *(shared)* Same gap as the AS5610: the
  daemon logs a table once a minute and there is no way to ask a running one
  anything.

### VXLAN, which the silicon and the SDK both already have

The switch does VXLAN routing, bridging and gateway at wire speed
(`EGR_VXLAN_CONTROL` is in the SDK's BCM56860 register database), and
`src/bcm/esw/trident2/vxlan.c` — 16,566 lines, 45 entry points — is already
compiled into the OpenBCM build NOSaic links against. NOSaic has nothing:
no VXLAN, no tunnel, no VTEP, and `switch-api` has no tunnel concept.

Behind ECMP and VRF, because neither of those is a feature for future users —
the box has two uplinks and uses one, and its management traffic follows a
learned route into the CPU path unless pinned by hand.

Two things to know before starting:

  - **L3 VXLAN routing needs recirculation** on this silicon — multiple passes
    through the forwarding pipeline, per Arista's 7050X architecture
    whitepaper. Bridging is free in the way line-rate features usually are;
    routing spends pipeline bandwidth. Measure it here rather than trusting
    the whitepaper.
  - **This is the contract's first real test.** Adding it means versioned
    additions to `switch-api` — tunnel endpoints, VNI-to-VLAN mapping, overlay
    next hops — with `nosd-virt` updated in the same commit so CI still
    exercises them, probably as Linux VXLAN interfaces over the veth
    dataplane. If the contract has to bend toward Broadcom to fit, the
    abstraction has failed and the fix belongs in the contract rather than in
    the test. That is the design's own stated criterion and nothing has
    exercised it yet.

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
