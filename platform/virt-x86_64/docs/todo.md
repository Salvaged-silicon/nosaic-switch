# virt-x86_64 — what is left

This board's job is different from a real one: it keeps `main` provable with no
switch attached, and it is where the `switch-api` contract was defined. So the
list below is mostly about **staying that**, not about becoming a switch.

If something here goes red, the core has grown a dependency on hardware it is
not supposed to have. That is the failure this board exists to catch.

## Required — the board is not doing its job without these

### Nothing outstanding

It boots under QEMU in CI on every commit, across all three profiles, and the
A/B upgrade, trial boot and automatic rollback are exercised there. That is what
this board is for and it is currently doing it.

## Nice to have

### ~~The CLI test is not yet the thing M6 measures~~ — it is now

The design says the M3 CLI test must pass **unmodified** against real silicon,
and that this is what proves the abstraction. It does: `nosaic show ports` and
`show caps` answer from a Trident2+ and from a Trident+ through the same
contract this board defined, with the speeds coming back from the chip rather
than from configuration.

It went further than the design asked. The AS5610 is 32-bit big-endian PowerPC,
which the Go toolchain has never targeted, so that switch runs a **second,
independent CLI written in C** — and the two were diffed against each other on a
board that can host either. `show caps` and `show ports` come back byte-for-byte
identical. The contract defined here against veth is now the thing two
implementations and two silicon families agree on.

### The virtual datapath has no capability model to disagree with

Every real ASIC will differ from this one, and the point of the capability model
is that the CLI reports an unsupported operation rather than quietly doing less.
With one implementation that always says yes, nothing exercises the reporting
path. A deliberately restricted second virtual profile would.

### Profiles are built but only compared by booting

CI builds `full`, `slim` and `minimal` and boots each. It does not check that
the same declaration produced equivalent services under systemd and under s6,
which is the actual claim `svcgen` makes.
