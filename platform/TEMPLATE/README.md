# <Board name>

One self-contained directory per board. Nothing outside it needs editing.

## Expected contents

| Path | What |
|------|------|
| `board.yml` | The board's identity and its three axes. Required. |
| `README.md` | This file: what the board is, how to get a console, how to install. |
| `portmap.yml` | Front-panel port names to physical and ASIC ports. The *only* place that translation happens. |
| `config/` | Kernel defconfig fragment, board-specific data files. |
| `dts/` | Device tree, on boards that need one. |
| `drivers/` | Out-of-tree kernel modules specific to this board. |

## Reverse engineering

If this board needed reverse engineering, link its repository here rather than
importing its contents. Keep vendor SDK source out of this tree — reference it
by file and line.
