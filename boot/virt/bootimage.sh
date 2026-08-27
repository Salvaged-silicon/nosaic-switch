#!/usr/bin/env bash
#
# Boot a composed NOSaic image under QEMU.
#
#   boot/virt/bootimage.sh <board>
#
# The image is attached as a disk and mounted by the initramfs, exactly as it
# would be on hardware. Nothing here is a shortcut around the real boot path.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
BOARD="${1:?usage: bootimage.sh <board>}"

board_field() { sed -n "s/^$2:[[:space:]]*//p" "platform/$BOARD/board.yml" | head -1 | tr -d '"'; }
arch_field()  { sed -n "s/^$2:[[:space:]]*//p" "arch/$1/arch.yml"          | head -1 | tr -d '"'; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

ARCH="$(board_field "$BOARD" arch)"
QEMU="$(arch_field "$ARCH" qemu_system)"
CONSOLE="$(arch_field "$ARCH" qemu_console)"
MACHINE="$(arch_field "$ARCH" qemu_machine)"
CPU="$(arch_field "$ARCH" qemu_cpu)"

DIR="out/images/$BOARD"
for f in vmlinuz initramfs.cpio.gz rootfs.sqsh; do
    [ -f "$DIR/$f" ] || die "$DIR/$f missing — run: make image BOARD=$BOARD"
done

LOG="$DIR/console.log"
echo "==> booting $BOARD under $QEMU"
set +e
timeout 180 "$QEMU" \
  ${MACHINE:+-machine "$MACHINE"} ${CPU:+-cpu "$CPU"} \
  -nographic -no-reboot -m 1024 \
  -kernel "$DIR/vmlinuz" \
  -initrd "$DIR/initramfs.cpio.gz" \
  -drive "file=$DIR/rootfs.sqsh,format=raw,if=virtio,readonly=on" \
  -append "console=$CONSOLE panic=5 loglevel=4 nosaic.selftest" \
  </dev/null >"$LOG" 2>&1
set -e

echo "--- console ---"
grep -aE 'NOSAIC-|Kernel panic|login:|Welcome' "$LOG" | sed 's/^/    /' | head -20 || true
echo "---------------"

grep -aq 'NOSAIC-INITRAMFS starting'    "$LOG" || die "the initramfs never ran; see $LOG"
grep -aq 'NOSAIC-INITRAMFS image mounted' "$LOG" || die "the image was not mounted; see $LOG"
grep -aq 'NOSAIC-INITRAMFS overlay assembled' "$LOG" || die "the overlay was not assembled; see $LOG"
grep -aq 'NOSAIC-BOOT userspace reached' "$LOG" || die "init never reached userspace; see $LOG"
grep -aq 'login:' "$LOG" || die "no login prompt appeared; see $LOG"
grep -aq 'NOSAIC-SELFTEST OK' "$LOG" || die "the self-test did not pass; see $LOG"
echo "OK — $BOARD boots, self-tests and powers off"
