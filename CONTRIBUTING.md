# Contributing to NOSaic

## Before anything else

```sh
make check
```

That runs formatting, static analysis, tests and the repository invariant checks — the
same thing CI runs. It needs only Docker and make.

## The rules that are enforced, and why

These are checked mechanically because a rule that depends on being remembered has
already been broken by the time anyone notices.

**Every recipe declares a license and whether it may be published.**
`license:` and `redistributable:` are both required, and both refuse omission rather than
defaulting. NOSaic is Apache 2.0, but built images may contain source-available vendor
code. The build will not produce a *publishable* image containing something marked
non-redistributable — it will still build it for local use.

**Sources are pinned by hash.** `source.sha256` is required and must be a full SHA-256.
Vendor sources are fetched at build time and never committed.

**A virtual name is exclusive.** A package declaring `provides: [nosd]` must also declare
`conflicts: [nosd]`. Without that, two datapath implementations can co-install and fight
over the same chip.

**Recipes never write init files.** Declare services in the `services:` stanza. Unit files
for systemd and definitions for s6/OpenRC are both *generated* from it, because the
minimal profile does not run systemd. A recipe that hand-writes a unit breaks the smallest
tier for everyone.

**Unknown keys are errors.** A misspelled field is a setting that silently never takes
effect, so the parser rejects it.

## Adding a board

Copy `platform/TEMPLATE/` to `platform/<your-board>/` and fill it in. The directory name
must match the `id` in `board.yml`. Nothing central needs editing — the catalog is derived
by scanning `platform/`.

Every board appears in the README's table with its status beside it, generated from the
board directories by `make docs` — `make check` fails if it has drifted. Hiding ports in
progress sounds honest and is not: somebody with that switch in a rack wants to know it
boots and is short of a few things, and a table that omits it says the project has never
heard of their hardware. Set `status:` to what has actually been demonstrated
(`planned` → `bringup` → `experimental` → `production`) and nothing is called `production`
until it has run somewhere that matters for longer than a lab afternoon.

## Reverse engineering

Undocumented hardware is a large part of this work, but it does not live here. Keep it in
its own repository per switch and link to it from the board's directory, so the OS tree
stays about the OS.

Never copy vendor SDK source into this repository. Reference it by file and line.
