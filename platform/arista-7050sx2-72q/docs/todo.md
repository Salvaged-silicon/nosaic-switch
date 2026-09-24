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

### A port can link and carry nothing until the datapath is restarted

Caught in the act on 2026-09-22, on Ethernet49, by the detector in
`tapbridge.c` — the first specimen with numbers rather than a recollection.

```
et49  link=1  tx-ok=12596  tx-err=0  out-uc=0  out-nuc=0  in-uc=0  in-nuc=0
                                     in-err=0  out-disc=0
et52  link=1  tx-ok=13524  tx-err=0  out-uc=137 out-nuc=13401   (the control)
```

On the healthy port `out-uc + out-nuc` tracks `tx-ok` exactly. On Ethernet49
the chip accepted **12,596 frames and egressed none**, reporting no transmit
error and no discard. The VLAN was right (`vid 1049 members 0 49 untagged 49`),
the address and route were installed, OSPF was up on the interface and sending
hellos, and the module reported no LOS. Nothing above the chip was wrong.

A `nosaic show ports` and log capture was taken first, as the detector's
message asks. Restarting the datapath cleared it immediately and completely:
the same port went to `in-uc=38 in-nuc=45 out-uc=42 out-nuc=54`, ARP resolved,
ping ran 4/4 at 0.44 ms, and OSPF reached Full.

**A new observation worth chasing.** This port was initialised in a state no
working port was: its cage was **empty at boot** — the optic went in afterwards
— and the far end was also down at the time, so it had neither a module nor a
link when the datapath came up. That is the same shape as the 7050TX-64's
late-cabled ports, which were fixed by making the bring-up level-triggered and
reconciled rather than done once at init. Worth testing directly: boot with a
cage empty, insert the optic, and see whether the port can ever transmit
without a restart.

**One theory tested and refuted, 2026-09-22.** The tapbridge header says the
transmit path ANDs its port bitmap with a link bitmap and "returns success
having built no descriptor", and `soc/common/link.c:soc_link_fwd_set`
maintains that as `EPC_LINK_BMAP` — a hardware memory in IPIPE that gates
egress. A port missing from it would produce this exact signature: descriptor
built, `tx-ok` counted, frame discarded before the MAC, no counter anywhere.

`nosaic platform linkmap` was written to read it, and it does:

```
EPC_LINK_BMAP = ffffffff 2223ffff 00000022 00000200
  -> logical {0..49} + {53,57,61,65,69} + 105
```

**Every configured port is set, including two cages with no module and no
link.** The bitmap is not maintained per-link on this board, so no bit is ever
missing and a missing bit cannot be the cause. The theory is dead, measured
rather than argued.

That leaves the empty-cage-at-boot circumstance above as the live hypothesis,
and `linkmap` as a standing check: if a port ever IS absent from the bitmap
while its interface reports up, the fault is named outright.

Until then the detector is the mitigation — it names the port and says the
restart clears it, which is the difference between a five-minute fix and the
day this cost when it was mistaken for a dead far end.

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

**Done 2026-09-24, on this board.** network.conf takes `vrf mgmt table 1001`,
then `vrf mgmt` on eth0's `iface` and `route` lines (see
`config/network.conf.example`), and the pin is gone. After a cold boot with the
new SWI (slot b, committed by its own trial):

- eth0 is `master mgmt`. Table 1001 holds eth0's v4 and v6 subnets and both
  defaults, and the main table holds only front-panel routes, with nothing
  `dev eth0` in either family.
- ssh answers on eth0 (`tcp_l3mdev_accept=1`). A 20 MB pull over it takes
  3.52 s against 3.56 s with the pin, so the rate is the pinned one, not 21 KB/s.
- Four OSPF adjacencies are Full, and 13 routes are in DEFIP with 0 failed.
  FRR sees the VRF (`show vrf`: `vrf mgmt id 4 table 1001`).
- From the default VRF the build host is "Network is unreachable". Inside
  (`ping -I mgmt`) it answers, and so does the v6 gateway.

What this did not reproduce is the original failure. OSPF is not carrying the
build network today, so nothing competes with eth0 for it. With the VRF that
failure cannot recur, because OSPF's routes go into a table eth0's traffic
never consults. The kernel is outside the A/B slot, and the SWI from before
this change is on flash as `nosaic-ab-prevrf.swi`.

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

### The signal repeater is never programmed, and works anyway

The board carries one TI repeater — the FDL gives `repeaterSmbus` at accel 1
bus 7 and `repeaterInfo = [ ( 'qsfpDS125BR401R1', 0x58 ) ]` — and it sits in
front of the **last two QSFP cages**, Ethernet53 and Ethernet54, the same
arrangement the 7050TX-64 uses for its DS100KR800.

NOSaic has the machinery: an s6 `retimer` service and
`nosaic platform retimer --program`. It runs at every boot and does nothing,
because `/etc/nosaic/retimer.conf` does not exist and this board has no
`mkretimer.sh` to generate one. The 7050TX-64 has both — copy its generator.

Measured on 2026-09-20: the part is populated and holds `EQ=0x2f`,
`amplitude=0xad`, `de-emphasis=0x82` on every channel base. Those do **not**
match the FDL's own `repeaterSettings` (`rxEqualization=0`,
`outputAmplitude=168`, `txDeEmphasis=0`), so they are most likely the
power-on defaults rather than anything EOS left behind — an earlier note
claiming they were EOS's was not supported by the numbers.

Both cages it serves carry Full adjacencies, so this is **unobserved rather
than broken**. It is still worth closing: two of the six 40G cages depend on
configuration that nothing in this codebase owns or can reproduce, and the
failure mode if it ever resets is the silent one — a cage that links and
carries nothing.

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

### ~~Two QSFP macros are left at the global lane map~~ — resolved 2026-09-21

Every one of the 18 cores now carries an explicit per-port lane map and no
global key remains. See *Fixed on 2026-09-21* below: the exceptions that were
"tried and refuted" were correct values under a key the SDK never asked for.

## Fixed on 2026-09-21

- **The SerDes lane map key hardcoded core 0, and two cages never got one.**
  `tsce.c` reads the lane map with `soc_property_port_suffix_num_get(unit,
  port, core_num, spn_XGXS_TX_LANE_MAP, "core", 0x3210)`, which builds
  `<name>_core<CORE_NUM>_<port>` and falls back to `<name>_<port>`. Every key
  in `asic.conf` was written `_core0_`, which is right only for ports whose
  core number really is 0. Logical 49 and 53 — Ethernet49 and Ethernet50 — ran
  on the SDK's own default map while the other four 40G cages got the values
  meant for them. The values were never wrong; the key reached four ports of
  six. Re-keyed without the infix, the way EOS writes it, which is correct
  whatever the core number is.

  **The symptom is why this took a day.** A wrong RX lane map brings the link
  UP — the lanes lock individually — and then nothing reassembles. `in-nuc`
  sat at 0 with `in-err` also at 0: not one corrupt frame, because nothing got
  far enough to be called a frame. From outside that is indistinguishable from
  a far end which is not transmitting, and the search went to the far end of
  the fibre, then the optics, then the retimer. The Nexus 3172 on the other
  side had been transmitting the whole time.

  **The daemon had been saying so at every boot.** `config N of M properties
  were NEVER read by the SDK` named `xgxs_tx/rx_lane_map_core0_49` and `_53`,
  and only those two of the six cages. That list is the first thing to read
  when a port links and carries nothing.

  Localised by booting EOS 4.18.3 on the same board, cage, optic and fibre:
  it brought Ethernet50 up at 40G where we received nothing, which cleared the
  hardware, the fibre and the far end in one step. Its `config show` then gave
  both the values and — the half that mattered — their correct spelling.

- **`tools/mkpolarity.sh` now captures the lane maps and firmware modes**, not
  just polarity, so this cannot drift again. Its absence is warned about
  separately, because a board with no lane maps still produces a file that
  looks complete. The 7050TX-64's generator already did this; ours did not.

- **The QSFP polarity table was wrong for four of six cages.** The generator
  that made it read lane 0 only, so cages whose lane 0 is not inverted were
  absent entirely — and absent reads as "no flip needed" rather than "never
  measured". The value is a bitmask over the cage's four lanes. Corrected and
  confirmed three independent ways: a live EOS register sweep, the board's own
  FDL, and EOS's running SDK config. This was a real latent bug but it was
  *not* what kept Ethernet49 and Ethernet50 dark.

- **`nosaic platform transceivers` reported "no signal" on working links.**
  These CISCO-AVAGO BiDi modules advertise receive-power monitoring in
  SFF-8636 byte 220 and then populate none of it. EOS reads the same zeroes on
  a link that is up, so it is the module, not our decode. An all-zero
  diagnostic block is now called what it is.

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

- **~~Two taps with the same name crash-loop the daemon~~ — fixed.**
  `/etc/nosaic/asic.conf` and `/mnt/data/config/asic.conf` are layers where the
  second overrides the first *per key*. Lookups always worked that way --
  `nosaic_props_get()` searches the table backwards -- but the table was
  append-only, so every caller that ENUMERATES it walked forwards and saw both
  copies. The same `tap_et52=` in each file therefore created the tap twice,
  the second `TUNSETIFF` returned `EBUSY`, the daemon exited, and s6 restarted
  it into the same wall eleven times.

  It bit because a persistent override was written for a tap the image did not
  yet ship, and the next image *did* ship it -- the ordinary lifecycle of an
  override, not a mistake anyone would notice making.

  Fixed in `datapath/common/props.c`: a redefined name now replaces the earlier
  entry at load time rather than being appended beside it, so every consumer
  gets last-wins for free and a new enumerator cannot reintroduce the bug by
  forgetting. `datapath/common/props_test.c` covers it and was checked against
  the unfixed file first -- three of its five assertions fail there.

  ⚠ Worth remembering how it presented, because none of it pointed at config:
  the log ends mid-startup with no error line, every cycle looks like a normal
  boot, and `nosaic show ports` reports only that `/run/nosd.sock` is missing --
  which reads as the chip failing to come up. `grep TUNSETIFF` was the only
  thing that said otherwise.

- **~~`upgrade install` writes the slot where the bootloader cannot find it~~ —
  already fixed; this board was running an old CLI.** With no `--disk` it wrote
  `/mnt/data/nosaic-slot-b.sqsh`, while the initramfs looks for a partition
  label, then `$FLASH/<slot>.sqsh`, and only then `/mnt/data`. With a stale slot
  file already on flash the new image is silently ignored and the box boots the
  old one.

  This was **fixed in `0b7b105` on 2026-09-12** -- `Local()` now asks where the
  slot it is RUNNING FROM lives and installs beside it. The failure was seen
  here only because the switch was running the CLI from slot a's image, built
  2026-09-11, one day earlier. The image now in slot b carries the fix.

  No code change. Recorded because the workaround is worth knowing if an old
  CLI is ever in play again: compare the install's output path against
  `slotdev=` in `/mnt/data/boot/log` before rebooting, and copy the image to
  `/mnt/flash/nosaic-slot-<x>.sqsh` if they disagree.

- **~~A datapath restart leaves the switch with no interface addresses~~ —
  fixed.** The taps belong to `nosd`; when it restarts they are destroyed and
  recreated **bare**, and `lo`'s own address goes with them.

  `network-config` is ordered `after nosd` precisely so s6-rc stops and re-runs
  it with the datapath -- and that works, for an `s6-rc` transition. It does
  nothing for the case that actually happens unattended: `nosd` is supervised
  with `restart: always`, so after a crash the **supervisor** restarts it
  directly and s6-rc is never involved. The oneshot stays marked done from boot
  while every address it configured has gone.

  A switch in that state boots correctly, runs for days, and silently stops
  routing at a moment nothing logged. Observed twice, read as something else
  both times -- once as expected behaviour, because
  [the walkthrough](walkthrough.md) describes the manual case as though it were
  the whole story.

  Fixed with a `network-reconcile` longrun: `apply-network.sh` gained a
  `NOSAIC_NET_RECONCILE` mode that keeps asking what the state IS and puts back
  whatever is missing, rather than waiting for an event that the supervisor
  never emits. Same conclusion the 7050TX-64's port hotplug reached --
  level-triggered, and reconcile on a timer, because an event can be missed
  entirely and the state cannot. A pass with nothing to do prints nothing, so a
  healthy switch shows no periodic noise.

## Features — what this board could do and does not yet

Shared with the AS5610 where marked *(shared)*: both run `datapath/common`, so
one fix lands on both. The AS5610's list is
[here](../../edgecore-as5610-52x/docs/todo.md#features--what-this-board-could-do-and-does-not-yet).

### Forwarding

- **~~ACLs~~ proven here, CoPP still to do.** *(shared)* The ACL model was
  built and proven on the AS5610 on 2026-09-16, and this board proved it
  portable on 2026-09-17: `nosd-td2p` links `datapath/common/acl.c`, and on
  the running switch a `deny in et52 proto icmp src <neighbour>` dropped 5 of
  5 pings and counted 5 in the chip, a permit above it restored them, a rule
  scoped to et53 left et52 traffic untouched (the port is in the key as
  SrcPort, so the AS5610's InPorts-pipeline quirk does not arise), and all
  three OSPF adjacencies stayed Full throughout. The Trident2+ field groups
  are far larger than the Trident+: `show caps` reports 10240 IPv4 and 4096
  IPv6 rules. IPv6 is not yet traffic-tested here (no v6 neighbour on an et
  port). CoPP is the next thing on top: a meter per rule and a CPU queue per
  class.
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
