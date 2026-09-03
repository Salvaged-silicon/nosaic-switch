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

Hashes are the first 16 hex characters, which is enough to notice a document
being replaced under the same URL. `make datasheets` prints the full hash of
what it fetched and does not fail on a mismatch: a vendor revising a datasheet
is normal, and the right response is to read the new one, not to stop the
build.

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
