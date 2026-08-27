# Building an image for <Board>

> Delete this line when the page is filled in.

Only what is specific to this board. The general build — toolchain, packages,
image, VM — is in [docs/BUILDING.md](../../../docs/BUILDING.md), and repeating
it here means it will drift.

## The short version

```sh
make toolchain ARCH=<arch>
make packages ARCH=<arch> PROFILE=<profile>
make image BOARD=<board>
```

The artifact this produces, and where it lands.

## What this board needs that the generic build does not

Firmware blobs, an out-of-tree module, a device tree, a defconfig fragment.
For each: where it comes from, whether it may be redistributed, and what
happens if it is missing.

## Profile

Which profile this board uses and why — usually flash size or RAM.

## Verifying before you install

Any check worth running on the artifact before putting it on hardware. On a
board where a bad image means a recovery session with a console cable, this
section earns its place.
