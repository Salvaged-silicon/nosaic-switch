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

### The CLI test is not yet the thing M6 measures

The design says the M3 CLI test must pass **unmodified** against real silicon,
and that this is what proves the abstraction. Here it passes against veth. It
has never been run on a switch, so the half of the claim that matters is
untested — see
[the 7050SX2's list](../../arista-7050sx2-72q/docs/todo.md#the-nosaic-cli-has-never-been-run-against-silicon).
The work is on that board, but the test lives here.

### The virtual datapath has no capability model to disagree with

Every real ASIC will differ from this one, and the point of the capability model
is that the CLI reports an unsupported operation rather than quietly doing less.
With one implementation that always says yes, nothing exercises the reporting
path. A deliberately restricted second virtual profile would.

### Profiles are built but only compared by booting

CI builds `full`, `slim` and `minimal` and boots each. It does not check that
the same declaration produced equivalent services under systemd and under s6,
which is the actual claim `svcgen` makes.
