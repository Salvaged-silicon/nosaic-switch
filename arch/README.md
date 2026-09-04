# arch/ — per-CPU support

One directory per CPU architecture, naming the toolchain and any ABI notes that
belong to the CPU rather than to a board.

`x86_64` and `aarch64` arrived at M1. **PowerPC (e500v2) is here and running**: it
is big-endian and soft-float, with an instruction audit that fails any build
containing an opcode an e500v2 cannot execute, and the AS5610-52X boots from its
own disk on it. armhf is still to come.

One consequence of that architecture is worth knowing before adding another. The
Go toolchain has `ppc64` and `ppc64le` and has never had 32-bit big-endian
PowerPC, so a board on it cannot run the Go CLI and runs the C one in `cli/`
instead. An architecture the compiler cannot reach is not a reason for a switch to
be operated differently: both implement the same commands against the same
contract, and they are checked against each other on a board that can host either.
