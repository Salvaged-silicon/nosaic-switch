# <Board name>

One self-contained directory per board. Nothing outside it needs editing.

## Expected contents

| Path | What |
|------|------|
| `board.yml` | The board's identity and its axes. Required. |
| `README.md` | This file: what the board is, its status, and links to its three pages. Required. |
| `docs/install.md` | Getting NOSaic onto this switch. Written for somebody holding it. Required. |
| `docs/build.md` | Building an image for it. Only what is board-specific. Required. |
| `docs/hardware.md` | The deep page: architecture, diagrams, port map, registers, quirks. Required. |
| `portmap.yml` | Front-panel port names to physical and ASIC ports. The *only* place that translation happens. |
| `config/` | Kernel defconfig fragment, board-specific data files. |
| `dts/` | Device tree, on boards that need one. |
| `drivers/` | Out-of-tree kernel modules specific to this board. |

## The three pages

Every board carries the same three, because a reader arrives with one of three
questions and should not have to guess which page answers it:

| Page | Audience |
|---|---|
| `docs/install.md` | Somebody holding the switch, with a console cable |
| `docs/build.md` | Somebody making an image for it |
| `docs/hardware.md` | Somebody changing the datapath or debugging silicon |

Copy them from `platform/TEMPLATE/docs/`. Each ships with a line marking it as
unfilled, and `nosaic check` fails while that line is still there — so a board
cannot merge with a page nobody wrote.

Nothing central is edited to add a board. `docs/switches.md` is generated from
these directories by `make docs`.

## Reverse engineering

If this board needed reverse engineering, link its repository from `README.md`
rather than importing its contents.

The line to hold: `docs/hardware.md` documents the board **as NOSaic drives
it** — topology, boot chain, port map, the register regions our code touches,
and the quirks worth somebody's afternoon. The *investigation* — traces,
hypotheses, eliminated leads, and anything derived from vendor binaries or
headers — stays in the RE repository. Vendor SDK source is never copied into
this tree; reference it by `file:line`.
