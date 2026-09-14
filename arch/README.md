# arch/ — per-CPU support

One directory per CPU architecture, naming the toolchain and any ABI notes that
belong to the CPU rather than to a board.

`x86_64` and `aarch64` arrived at M1. **PowerPC (e500v2) is here and running**: it
is big-endian and soft-float, with an instruction audit that fails any build
containing an opcode an e500v2 cannot execute, and the AS5610-52X boots from its
own disk on it. **armhf is declared and has never been built** -- it is the
AS4610-54T's dual Cortex-A9, and its arch.yml says plainly that nothing on it
has been through a compiler yet.

The two 32-bit architectures make the same point from opposite ends, and it is
worth stating once here rather than twice below. Both carry an instruction
audit, because on both the sample the toolchain would naturally pick describes
a CPU slightly better than the one in the rack: a classic FPU the e500v2 does
not have, a NEON unit this Cortex-A9 does not have. A build against either
compiles, links, disassembles and runs under an emulator. The audit is what
notices, and it is the reason a new architecture here starts by reading
`/proc/cpuinfo` off the actual switch rather than by naming a part number.

One consequence of that architecture is worth knowing before adding another. The
Go toolchain has `ppc64` and `ppc64le` and has never had 32-bit big-endian
PowerPC, so a board on it cannot run the Go CLI and runs the C one in `cli/`
instead. An architecture the compiler cannot reach is not a reason for a switch to
be operated differently: both implement the same commands against the same
contract, and they are checked against each other on a board that can host either.
