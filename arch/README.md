# arch/ — per-CPU support

One directory per CPU architecture, naming the toolchain and any ABI notes that
belong to the CPU rather than to a board.

Populated at M1 with `x86_64` and `aarch64`. PowerPC (e500v2) and armhf arrive at
M8, when older hardware is bolted in.
