# Building an image for virt-x86_64

The general build — toolchain, packages, image, VM — is in
[docs/BUILDING.md](../../../docs/BUILDING.md). Only what is specific to this
board is here.

## The short version

```sh
make toolchain ARCH=x86_64
make packages ARCH=x86_64 PROFILE=minimal
make pkg PKG=linux ARCH=x86_64
make image BOARD=virt-x86_64
```

This produces `out/images/virt-x86_64/`: `vmlinuz`, `initramfs.cpio.gz`,
`rootfs.sqsh` and `disk.img`. The `virt` boot backend emits no installer, because
QEMU is handed the kernel, initramfs and disk directly.

## What this board needs that the generic build does not

Nothing. That is deliberate and worth stating: this board exists to prove the
core builds with no board-specific anything. No firmware blobs, no out-of-tree
modules, no device tree, no defconfig fragment. If this board ever acquires one,
the boundary it is meant to protect has already moved.

## Profile

`minimal` — glibc, busybox and s6. Not because the VM is short of space, but
because the minimal profile is the one whose init backend is easiest to get
wrong. Building it here every commit is what keeps the abstract `services:`
stanza honest: a recipe that reached for a systemd-specific feature would break
this board first.

## Verifying before you run it

```sh
make image-boot BOARD=virt-x86_64   # boots twice, self-tests, powers off
make image-ab   BOARD=virt-x86_64   # upgrade commits, and rolls back when broken
make dataplane-test                 # real veth interfaces, driven through switch-api
```

`image-boot` boots twice on purpose. The second boot is what proves the data
partition is real — a tmpfs pretending to be persistent passes everything a
single boot can check.
