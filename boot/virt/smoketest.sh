#!/usr/bin/env bash
#
# Boot a NOSaic kernel under QEMU and check it can run NOSaic userspace.
#
#   boot/virt/smoketest.sh <arch>
#
# The kernel comes from the built package rather than from a build tree, so
# what is tested is the artifact that would actually ship.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ARCH="${1:?usage: smoketest.sh <arch>}"

arch_field() { sed -n "s/^$2:[[:space:]]*//p" "arch/$ARCH/arch.yml" | head -1 | tr -d '"'; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

TRIPLE="$(arch_field "$ARCH" triple)"
QEMU="$(arch_field "$ARCH" qemu_system)"
CONSOLE="$(arch_field "$ARCH" qemu_console)"
MACHINE="$(arch_field "$ARCH" qemu_machine)"
CPU="$(arch_field "$ARCH" qemu_cpu)"
[ -n "$QEMU" ] || die "arch/$ARCH/arch.yml has no qemu_system: this architecture cannot be booted here"

PKG="$(ls -1 out/packages/linux_*_"$ARCH".nos 2>/dev/null | head -1)"
[ -n "$PKG" ] || die "no kernel package for $ARCH — run: make pkg PKG=linux ARCH=$ARCH"

WORK="$ROOT/.cache/smoketest-$ARCH"
rm -rf "$WORK"; mkdir -p "$WORK/initramfs"

echo "==> unpacking $(basename "$PKG")"
tar -xOf "$PKG" data.tar.gz | tar -xzf - -C "$WORK" ./boot 2>/dev/null || \
  tar -xOf "$PKG" data.tar.gz | tar -xzf - -C "$WORK" 2>/dev/null
KERNEL="$(find "$WORK" -name 'vmlinuz-*' -o -name 'vmlinux-*' | head -1)"
[ -n "$KERNEL" ] || die "the package contains no kernel image"

# init is built with this architecture's own toolchain, so a successful boot
# also proves the toolchain's output runs on the kernel it was built alongside.
echo "==> building init with $TRIPLE-gcc"
CC="toolchain/$ARCH/bin/$TRIPLE-gcc"
[ -x "$CC" ] || die "$CC not found — build the toolchain first"
"$CC" -static -O2 -o "$WORK/initramfs/init" boot/virt/test/init.c

echo "==> packing initramfs"
( cd "$WORK/initramfs" && find . | cpio -o -H newc --quiet | gzip -9 -n ) > "$WORK/initramfs.cpio.gz"

echo "==> booting under $QEMU"
LOG="$WORK/console.log"
set +e
timeout 120 "$QEMU" \
  ${MACHINE:+-machine "$MACHINE"} ${CPU:+-cpu "$CPU"} \
  -nographic -no-reboot -m 512 \
  -kernel "$KERNEL" \
  -initrd "$WORK/initramfs.cpio.gz" \
  -append "console=$CONSOLE rdinit=/init panic=1 loglevel=4" \
  </dev/null >"$LOG" 2>&1
set -e

echo "--- console ---"
grep -E 'NOSAIC-|Linux version|Kernel panic' "$LOG" | sed 's/^/    /' || true
echo "---------------"

grep -q 'NOSAIC-INIT' "$LOG" || die "userspace never started; see $LOG"
if grep -q 'NOSAIC-FAIL' "$LOG"; then
    die "the kernel booted but is missing something it was configured with; see $LOG"
fi
grep -q 'NOSAIC-OK' "$LOG" || die "the boot did not complete; see $LOG"
echo "OK — $ARCH boots and runs its own userspace"
