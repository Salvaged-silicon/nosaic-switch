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

- **~~et1 and et2 are configured against nothing~~** — removed on 2026-09-11.
  Both were admin up with an address and no carrier, and neither far end
  (`10.101.101.41`, `.58`) ever answered. Two costs, not one: `show ports` and
  `ip addr` both read as a cabling fault on two ports that are simply not
  cabled, and FRR advertised each /29 into the area the moment it saw an
  address on an up interface, so OSPF carried two prefixes no traffic could
  reach. The addresses are in `config/network.conf`'s comment and in the git
  history; putting either back is one line.

### The MAC address is hard-coded

`config/network.conf` states `mac 44:4c:a8:eb:93:f6` because the board keeps it
in the SCD mailbox rather than anywhere `tg3` can find it. Reading the `prefdl`
would settle this and also gives board identity and `Trident0CoreVdd`. As
written, this file is correct for exactly one switch.

### The full profile has never been booted here

`board.yml` says `profile: full` (systemd). Only `minimal` (s6) has run on this
board. Either boot it or change the declaration; a board description that
disagrees with reality is worse than either.

### ~~ECMP~~ — proven here on 2026-09-11, in hardware

`l3sync` read routes from `/proc/net/route`, which lists one gateway per prefix,
so a multipath route was programmed as a single path and reported success. It
now reads them over netlink and builds ECMP groups, and the multipath hash is
configured -- without that the group exists and sends everything down one
member anyway.

Proven on the AS5610 first: 150 transit packets across 30 destinations, 80 and
70 across the pair.

**Now proven here too, and it only became testable when Ethernet52 came up.**
The blocker recorded here was real -- this box's uplinks had different costs, so
nothing offered it an equal-cost route. et52 and et53 both reach the
7050TX-64, both at 40G, both Full, so the box finally had a pair:

```
O>* 10.101.107.0/24 [110/20] via 10.101.101.74, et53, weight 1
  *                          via 10.101.101.82, et52, weight 1
```

Six prefixes carry two next hops, both resolved to egress objects on different
ports, and the chip built one group serving all of them:

```
l3: next hop 10.101.101.74 dev et53 via 02:1c:73:00:00:35 -> egress 100007
l3: next hop 10.101.101.82 dev et52 via 02:1c:73:00:00:31 -> egress 100007
l3: ecmp group of 2 -> egress 200256
```

And it splits. 75 transit packets across 15 destinations, from the AS5610:

| port | before | after | forwarded |
|---|---|---|---|
| et52 | 38 | 78 | **40** |
| et53 | 34 | 69 | **35** |

40 + 35 = 75, against 75 sent. Exact, so nothing was lost or double-counted.

Two things the method needed, both worth knowing before repeating it:

  - **CPU-originated traffic does not test this.** The kernel picks the next
    hop and hands the packet to one tap, so the chip's group is never
    consulted. Only traffic the chip *forwards* exercises the hash, which
    means a source behind another switch.
  - **Nothing sends this box transit, by design.** `max-metric router-lsa` is
    deliberate, and the AS5610 confirmed it by routing around us. The test
    needed a temporary static route on the AS5610 pointing one prefix at our
    et54, removed afterwards; its routing was checked back to its original
    state.

The split is per-flow and hashes on the destination, so it is lumpy with few
flows and evens out with more -- at the halfway mark this run stood at 35/10
and finished at 35/40. Do not read a small sample as a broken hash.

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

## Fixed on 2026-09-11

- **The DMA pool leaked here too, and had not been noticed.** This board's
  `sal_dma_alloc` bumped a pointer and its `sal_dma_free` did nothing, the same
  design and the same reasoning as the AS5610's separate copy. `bcm_tx` takes a
  DMA vector per transmitted packet and gives it back, so every packet the
  control plane sent cost the pool 1408 bytes for ever.

  It was **found on the AS5610**, which exhausted 64 MiB in a few hours and
  spent the rest of its uptime failing every allocation about 200 times a
  second with its control plane down. Nothing had been observed here, which is
  not the same as nothing happening: this board RAM-boots with a larger
  reservation and had not been left up long enough under control-plane traffic.
  The bug was identical.

  Both boards now share one allocator -- `datapath/common/dmapool.c`, first fit
  with coalescing, under a mutex -- rather than a copy each. The reasoning, the
  measurements and what the SDK actually does are in
  [the AS5610's todo](../../edgecore-as5610-52x/docs/todo.md), which is where it
  was found; this entry exists so the fix is not invisible from the board that
  also carried the bug.

  `nosaic show dma` reports the pool and, per allocation name, what is
  outstanding. A name whose total climbs with uptime is a leak. That table
  exists because working out which caller had taken the pool the first time
  meant reading Broadcom's source rather than asking the switch.

  **Measured here on 2026-09-11, and it was not hypothetical.** The A/B
  upgrade left the old image in slot b and the fixed one in slot a, so the two
  logs sit side by side on the same switch:

  | | old image (slot b) | fixed (slot a) |
  |---|---|---|
  | `DMA pool exhausted` lines | **703** | **0** |
  | pool | `67107520 / 67108864` -- full | 8.3 MiB of 64, peak 8.9 |
  | **chip FIB** | **`CHIP route 0/8192`**, every sample | `CHIP route 15/8192` |
  | unresolved next hops | climbing, 62760 -> 62940 | 240, steady |

  **The third row is the one that matters.** This was not log noise: the box
  had stopped programming routes into the ASIC altogether -- zero routes in
  hardware, in all 100 samples, while it still linked, still held its OSPF
  adjacency and still answered ping. A switch in that state looks entirely
  healthy from outside and forwards nothing it has to route.

  Worth contrasting with the AS5610, where the same bug left silicon
  forwarding intact and only stopped the control plane transmitting. Already
  programmed forwarding needs no CPU DMA; installing it does. Which symptom
  you get depends on whether the pool ran out before or after the routes went
  in, so this bug does not have one signature to look for.

  The callers named at the wall here were `fp_64_bit_counter` (602) and
  `l2 traverse` (101) -- the periodic collectors, which allocate every
  interval and, with `free` doing nothing, never gave one back. The AS5610's
  write-up names `bcm_tx` instead. Both leak; whoever asks *after* the pool is
  full is who gets named, so the caller in the message is not the culprit.

  The entry above explained the absence of observations here by this board
  RAM-booting with a larger reservation. That half is now out of date -- it
  installs to flash and boots from a slot -- which is likely why it finally
  showed: ordinary pool, ordinary uptime.

- **`make image` shipped stale binaries, and only warned about it.** The image
  build composes whatever is in `out/packages/`, which is right. But three
  recipes build from directories in this repository, and for those "already
  built" and "current" are different things: editing `datapath/td2p/*.c` and
  running `make image` produced an image containing the **previous** binary.

  A warning had been added for it and was not enough -- it scrolled past in a
  long build log on the day it was written. The build now **refuses**, names the
  file that changed and the `make pkg` line that fixes it, and takes
  `--allow-stale` for the times you mean it (bisecting, or pairing today's image
  with yesterday's datapath).

  The warning was also too coarse to trust. `nosd-td2p` and `nosd-tdp` both
  declare `source: local: datapath` and differ only by `build.subdir`, so
  editing the **AS5610's** daemon marked this board's package stale -- for a
  file its build never compiles. The check now follows what the recipe actually
  builds: its own subdir plus `datapath/common`, and not the other board's. It
  counts the recipe itself as source, since that carries the compiler flags and
  the staging, and it does not count a build's own output in the tree, which
  would otherwise make every package look stale the moment it was built.

  Proven against this tree in all three directions: clean when fresh, silent
  when `datapath/tdp` is touched, refusing when `datapath/common/dmapool.c` is.

  It is mtime-based, so a `git checkout` that rewrites timestamps can still make
  a package look stale when it is not; the flag covers that and a content hash
  would remove it. Worth doing when it becomes annoying rather than before.

- **The on-box CLI reported the wrong version, and always had.** The image
  said `0.1.0` -- `/etc/nosaic/image.json` and the SWI's `version` file both --
  while `nosaic version` on the same switch said `0.0.0-dev (unknown)`.

  `internal/imgbuild/cli.go` compiled the CLI during the image build with
  `-ldflags "-s -w"` and never passed the `-X …version.Version=` stamp. The
  Makefile's `LDFLAGS` reaches `go build`, `go run` and `pkg build`, so every
  build-host path was stamped and the one binary that actually ships was not.
  Nothing caught it because the CLI is not a package -- it is pure static Go,
  built in-tree rather than from a recipe, so it sits outside the packaging
  that commit `50ecc36` taught to stamp itself.

  It reads as cosmetic and is not: the first question after an A/B upgrade is
  which image you are on, and an unstamped CLI answers `0.0.0-dev` from both
  slots.

  Fixed, and verified without touching the switch -- the CLI was extracted
  from the newly built `rootfs.sqsh` and run on the build host:

      nosaic 0.1.0 (166e910)

  A `-X` naming a package that has moved fails silently: the build succeeds
  and the variable keeps its default, which is how this went missing in the
  first place. So the path is a stated constant and a test checks that it
  still resolves to a real package declaring `var Version` and `var Commit`,
  rather than trusting the string.

- **Ethernet52 was dark because its SerDes polarity was wrong, and the
  generator could never have found it.** Cabling after the move is Et52 ->
  7050TX-64 port 49, Et53 -> port 50, Et54 -> AS5610 port 51. Et52 was not
  configured at all; it now is -- `tap_et52=61:1052:1600` and
  `10.101.101.81/29`, the next free /29 after et53's, this end taking the
  lower address the way et53 and et54 do.

  **The cage number is not the logical port.** Ethernet52 is logical port 61
  (physical 81) -- the cages are not in physical order, and `portmap_53` is
  Ethernet50. `tap_et*` takes the logical port. EOS agrees: its `ps` shows
  `xe60( 61) up 40G` for this cage, so the port map was never in question.

  It then linked admin-up at 40000 and stayed **oper down**, with the far end
  reporting no light from us. Everything obvious was correct: cage 52 out of
  low power and reset, TX_DISABLE clear and demonstrably controllable, and the
  ASIC port initialising identically to the two cages that work -- same speed,
  same ability word, same interface, no errors.

  **Booting EOS settled it in one command.** `Et52/1 connected 40G` on the same
  module, the same fibre and the same far-end port. So the hardware was fine
  and the fault was ours. Reading the SerDes polarity out of the running EOS
  for the three 40G macros gave:

  | cage | macro | EOS | NOSaic shipped | |
  |---|---|---|---|---|
  | Et53 (61→65) | `0x181` | tx `0x01` rx `0x04` | tx `0x1` rx `0x4` | match |
  | Et54 (69) | `0x18d` | tx `0x0e` rx `0x0b` | tx `0xe` rx `0xb` | match |
  | **Et52 (61)** | `0xf1` | **tx `0x0d` rx `0x0b`** | tx `0x1` rx `0x1` | **wrong** |

  Corrected through `/mnt/data/config/polarity.conf`, and **et52 came up at
  40000 on the next datapath restart.**

  **Why the generator missed it, which is the part worth keeping.**
  `tools/mkpolarity.sh` read the lane out of the PHY name and filtered on
  `lane ~ /^[0-3]$/`. A 40G port names its lane **`4`** -- not a lane number,
  it means the whole macro -- so **every 40G port was silently dropped from the
  table**. The values that were in the file came from a capture taken while
  these cages were broken out into 4x10G, where they did appear as lanes 0..3;
  correct for that configuration and wrong once the cage was a single 40G port.
  Et53 and Et54 happened to survive it. Et52, never cabled then, did not.

  Worse than the filter: even had a 40G port passed, the script emitted `0x1` --
  a single bit -- where the value is a bitmask over the port's lanes and a 40G
  port needs up to four. Both fixed: the generator now takes lane `4` as lanes
  0..3 and builds the mask per lane.

  This is the failure mode the polarity work has always had, and it stayed true
  here: wrong polarity does not announce itself. On a single-lane 10G port it
  brings the link **up** carrying garbage. On a four-lane 40G port it prevents
  link entirely, which reads exactly like a dead cage, a bad optic or a far-end
  fault -- and it was diagnosed as all three before EOS ruled them out.

  Left over: the far side of et52 (`10.101.101.82`) is not configured yet.
  `et1` and `et2` were also carrying addresses and OSPF networks for links
  whose far ends had never answered; those were removed on 2026-09-11.

## Features — what this board could do and does not yet

Shared with the AS5610 where marked *(shared)*: both run `datapath/common`, so
one fix lands on both. The AS5610's list is
[here](../../edgecore-as5610-52x/docs/todo.md#features--what-this-board-could-do-and-does-not-yet).

### Forwarding

- **ACLs and CoPP.** *(shared)* It went the other way round: the ACL model was
  built and proven on the AS5610 on 2026-09-16 (its field processor worked all
  along, see its todo), and this board is the one that has to prove it
  portable. `nosd-td2p` links `datapath/common/acl.c`; nobody has run
  [the test](../../../docs/acl.md#what-was-measured) here yet. CoPP is then a
  meter per rule and a CPU queue per class on top of it.
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
  Cheaper than it was: the pool reclaims now, so a sweep that allocates and
  frees costs nothing permanent.
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
