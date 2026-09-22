# Arista DCS-7150S-52

52 × SFP+ at 10G, x86_64, Aboot — and an **Intel/Fulcrum FM6000 "Alta"** where
every other board in this tree has a Broadcom chip.

| | |
|---|---|
| Model | DCS-7150S-52 (`-CL-F` on the lab unit) |
| Silicon | Intel FM6000, PCI `8086:155b` (Fulcrum `1823:1770`) |
| Arch | x86_64 — AMD Family-10h embedded, RS780 + SB700/SB800 |
| Boots via | Aboot 2.1.0 ("norcal2") from a USB DOM |
| Front panel | 52 SFP+, 10G, no QSFP, no external PHYs |
| Status | **planned** — nothing has been built or booted for this board yet |

- [Installing](docs/install.md) · [Building](docs/build.md) ·
  [Hardware reference](docs/hardware.md) · [What is left](docs/todo.md) ·
  [What the prior work established](docs/edgenos-prior-art.md)

```
   nosaic CLI ──JSON──▶ /run/nosd.sock ──▶ nosd-fm6000 ──▶ mmap(BAR0) ──▶ FM6000
                                              ▲                │
   FRR ──▶ Linux IP stack ──▶ swp1..swp52 ────┘                │
                                     taps                      ▼
                        no SDK · no BDE · no CMIC · no vendor blob in the image
```

## Why this board is not like the others

NOSaic's datapath so far is four Broadcom ports that share a shape: a CMIC, a
userspace BDE over an mmap of BAR0, and the openbcm SDK doing the chip-specific
work above it. None of that exists here.

There is no BDE because there is no CMIC. There is no SDK because the only one
is `libFocalpointSDK.so`, which is proprietary and will not be linked, shipped
or copied into this repository. What replaces both is register-level code we
write ourselves against the FM6000's own blocks, driven by this board's port
map — which is the whole of the work, and the reason this board gets its own
`asic:` axis and its own `nosd` provider rather than an adaptation of one that
exists.

## The one thing that must not be copied in

There is prior art on this exact chassis, and it got a long way: the FM6000 has
been cold-booted and moved packets under another OS on this bench. It did it
with a **register replay** — several hundred thousand writes captured off EOS
and played back at the chip, with the parts we understood filled in around
them.

That is a fine way to prove a chip can be driven and a bad way to drive one.
A replay is a recording of one switch being configured one way on one day: it
cannot take a port map, it cannot be told to bring up 10 ports instead of 52,
nothing in it can be explained to the next person, and it carries whatever
licence the capture carries. **NOSaic does not replay, and does not warm-inherit
a chip EOS configured.** The register work is done properly here or it is not
done.

The trace is still evidence — the best evidence there is for what a working
FM6000 looks like. Using it as an *oracle* to check our own programming against
is right; emitting it is not.

And the hard line under all of it is **redistribution**. NOSaic stays a
repository anyone can take and an image anyone can publish: no vendor artifact
goes in it, whether that is a capture, a microcode blob or a table lifted out of
either.

That held even where it looked expensive. The FM6000's parser is microcoded, and
the obvious move was to ship Arista's blob under a non-redistributable recipe —
which would have made this the one board whose images could never be published.
It turns out not to be necessary: the parser's instruction encoding is public
(Intel document 331496-002, Table 5-3), so NOSaic **generates its own parser
microcode** and that blob is never in the picture. See
[docs/hardware.md](docs/hardware.md#the-parser-is-microcoded-and-we-write-the-microcode).

One firmware question is still open and could change this: SerDes bring-up goes
through a **SPICO** microcontroller, and whether its code is a separate vendor
file, lives inside the proprietary SDK, or is not needed on this part has not
been established. Until it is, "publishable" is the intent rather than a
demonstrated property.

## Reverse engineering

[docs/edgenos-prior-art.md](docs/edgenos-prior-art.md) records what the earlier
work on this chassis established, what NOSaic takes from it, and the two things
it deliberately does not.

The investigation itself lives outside this repository, per
[CONTRIBUTING](../../CONTRIBUTING.md): traces, disassembly, eliminated leads and
anything derived from vendor binaries stay there.
`docs/hardware.md` here documents the board **as NOSaic drives it** — and while
NOSaic does not yet drive it at all, that page states what is measured, what is
transcribed from the investigation, and what is still assumed.
