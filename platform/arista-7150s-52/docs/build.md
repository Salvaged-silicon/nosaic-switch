# Building an image for the Arista DCS-7150S-52

Only what is specific to this board. The general build — toolchain, packages,
image — is [docs/BUILDING.md](../../../docs/BUILDING.md).

```sh
make toolchain ARCH=x86_64
make image BOARD=arista-7150s-52
```

x86_64, so it shares the toolchain with the 7050SX2, the 7050TX-64 and the
Nexus; nothing needs building twice.

## What is different here

**The datapath has no SDK.** Every other board's `nosd` links against `openbcm`,
which is staged at build time and never shipped. This board's datapath is
`datapath/fm6000`, it links against nothing but libc and pthread, and the build
has no vendor tree to stage. That makes it the only datapath in the tree a
contributor can build in full with nothing fetched.

`libFocalpointSDK.so` is not a build dependency and must not become one. It is
proprietary, it cannot be redistributed, and it is useful here only as something
to compare our own programming against — outside this repository.

**The parser microcode is generated, not shipped.** The FM6000's parser is
microcoded, and NOSaic emits that microcode from its own generator rather than
carrying Arista's — the instruction encoding is public (Intel document
331496-002, Table 5-3). No vendor parser blob is fetched, staged or installed.

**One firmware question is open**, and it is the one that decides whether an
image for this board is publishable: SerDes bring-up goes through a SPICO
microcontroller, and whether its code is a separate vendor file, is embedded in
the proprietary SDK, or is unnecessary on this part is not yet known. If it
turns out to be a required vendor blob, this board gets a
`redistributable: false` recipe for it and its images stop being publishable.

See [hardware.md](hardware.md#the-serdes-firmware-question-is-not-settled).

## Board state

`status: planned`. `make image BOARD=arista-7150s-52` is not expected to produce
anything bootable yet; the board directory exists so the port has somewhere to
land.
