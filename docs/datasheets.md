# Vendor datasheets

The datasheets for the boards NOSaic runs on are not in this repository. They
are the vendors' copyrighted documents -- the Edgecore one carries "© Copyright
2014 Edge-Core Networks Corp." on its last page -- and this project does not
redistribute vendor material it has no licence to redistribute. That is the same
rule the recipes follow for vendor source: a URL and a hash, fetched on demand,
never committed.

`make datasheets` fetches them into `refs/`, which is ignored by git.

| board | document | sha256 |
|---|---|---|
| Edgecore AS5610-52X | [AS5610-52X-C datasheet](https://www.edge-core.com/_upload/images/1604211522050.pdf) | `b6ff46841cf92f2b…` |
| Edgecore AS5610-52X | [installation guide](https://www.edge-core.com/_upload/images/AS5610-52X_IG-R01_1220.pdf) | `8593074332e87113…` |
| Arista 7050SX2-72Q | [7050SX series datasheet](https://people.ucsc.edu/~warner/Bufs/7050SX-128_64_Datasheet.pdf) | `3be4a69b611520da…` |
| Arista 7150S-52 | [Intel FM5000/FM6000 datasheet, 331496-001 rev 3.3](https://people.ucsc.edu/~warner/Bufs/ethernet-switch-fm5000-fm6000-datasheet.pdf) | `ec88b44fd80a3fcb…` |

Hashes are the first 16 hex characters, which is enough to notice a document
being replaced under the same URL. `make datasheets` prints the full hash of
what it fetched and does not fail on a mismatch: a vendor revising a datasheet
is normal, and the right response is to read the new one, not to stop the
build.

## The FM6000 datasheet is different from the others

Every other row above is a brochure. The Intel one is a **programming
reference**, and the 7150S-52 port depends on it in a way no other board
depends on a datasheet: it documents the parser's microcode encoding (Table
5-3), the twelve-step cold boot (Table 4-1), the packet DMA engine and its
descriptor format (§7.11, Table 7-5) and the internal frame tag (Table 7-8).
That is the difference between writing a driver and guessing at one, and it is
why the FM6000 board can be ported with no vendor SDK at all.

**Two revisions exist and they are not interchangeable for citation.**

| | Revision | Document | Pages | sha256 |
|---|---|---|---|---|
| what `platform/arista-7150s-52/` cites | 3.4, July 2017 | **331496-002** | 352 | `b71f1b2b78476fb4…` |
| what `make datasheets` can fetch | 3.3, November 2014 | 331496-001 | 354 | `ec88b44fd80a3fcb…` |

Intel's own URL for -002 refuses an automated fetch — it answers `Access
Denied` with a 512-byte error page rather than the PDF — so the mirror above
serves -001. **Section and table numbers in the board's pages are -002's**, and
the two revisions differ by two pages, so check before assuming a number lines
up. If you have -002, its hash is in the table.

## What a datasheet is good for here, and what it is not

The board hardware references in `platform/*/docs/hardware.md` are written from
running units, and that is deliberate: what the silicon does and what the
brochure says diverge, and only one of them is going to be true at three in the
morning. The AS5610's fan duty register is five bits wide and the datasheet does
not mention it; its power supply status bits are documented nowhere and had to
be worked out against a running box.

Datasheets are good for the things a running unit will not tell you: absolute
maximum ratings, the operating temperature range, airflow direction, power
draw, what the SKU suffix means, and which optics the cages are specified for.
Read them for those. Do not take a forwarding-table size from one and assume
the running chip agrees -- the AS5610's does, at 16K IPv4 routes, but that was
checked.

The FM6000 document above is the exception that proves the rule rather than a
counter-example. It is trusted for the *encodings* -- what a microcode word's
fields are, what a descriptor looks like, what order the boot commands go in --
because those are the silicon's contract and there is nowhere else to get them.
It is not trusted for what the chip does when those are written, and the board's
own `hardware.md` marks anything read from it as `derived` until the running
unit agrees.
