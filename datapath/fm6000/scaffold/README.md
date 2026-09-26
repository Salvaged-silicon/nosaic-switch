# Scaffold: the blocks we have not derived yet

This directory holds **relocated vendor capture** — recorded register writes
from an EOS boot, moved into C arrays. It is how link gets working before the
real generators exist, and every file in it is meant to be deleted.

## ⚠ Nothing here is committed, and nothing here ships

The directory is gitignored. `fetch.sh` populates it from the EdgeNOS branch
on demand, and `make` in here builds a separate binary that no profile lists
and no image installs.

That is deliberate and it is the point of the arrangement. NOSaic's reason for
existing on this board is an image that can be redistributed; a tree carrying
130,000 lines of somebody else's captured boot cannot be, however it is
labelled. Keeping the scaffold out of the repository means the claim stays
true *while* the work is being done, rather than becoming true at the end if
the work finishes.

## Why scaffold at all

Deriving a block with nothing to test against is guessing with extra steps.
With the scaffold in place a lane **links**, so each derived block can be
swapped in and judged against something that works: if the link survives, the
derivation is right. That is the loop that took the prior investigation from
replaying everything to replaying 6.5% of it, and it is not available to
somebody starting from an empty tree.

## What is in scope

29 of the 41 blocks the link layer needs. The other 12 are authored code and
belong in `datapath/fm6000` proper, not here:

    safinit  cmrest  cmwm  esched  l3arslice1..4  l3artables
    parserfields  smalltables  tbl3init

## The rule

**A block leaves this directory by being replaced, never by being tidied.**
`manifest.txt` is the scoreboard; a block moves to `ported` only when its
replacement holds a link on hardware.
