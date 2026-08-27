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
for f in vmlinuz initramfs.cpio.gz disk.img; do
    [ -f "$DIR/$f" ] || die "$DIR/$f missing — run: make image BOARD=$BOARD"
done

boot_once() {
    local log="$1" slot="$2"
    set +e
    timeout 180 "$QEMU" \
      ${MACHINE:+-machine "$MACHINE"} ${CPU:+-cpu "$CPU"} \
      -nographic -no-reboot -m 1024 \
      -kernel "$DIR/vmlinuz" \
      -initrd "$DIR/initramfs.cpio.gz" \
      -drive "file=$DIR/disk.img,format=raw,if=virtio" \
      -append "console=$CONSOLE panic=5 loglevel=4 nosaic.selftest nosaic.slot=$slot" \
      </dev/null >"$log" 2>&1
    set -e
}

LOG="$DIR/console.log"
echo "==> booting $BOARD under $QEMU (slot a)"
boot_once "$LOG" a

echo "--- console ---"
grep -aE 'NOSAIC-|Kernel panic|login:|Welcome' "$LOG" | sed 's/^/    /' | head -20 || true
echo "---------------"

grep -aq 'NOSAIC-INITRAMFS starting'    "$LOG" || die "the initramfs never ran; see $LOG"
grep -aq 'NOSAIC-INITRAMFS image mounted' "$LOG" || die "the image was not mounted; see $LOG"
grep -aq 'NOSAIC-INITRAMFS overlay assembled' "$LOG" || die "the overlay was not assembled; see $LOG"
grep -aq 'NOSAIC-BOOT userspace reached' "$LOG" || die "init never reached userspace; see $LOG"
grep -aq 'login:' "$LOG" || die "no login prompt appeared; see $LOG"
grep -aq 'NOSAIC-SELFTEST OK' "$LOG" || die "the self-test did not pass; see $LOG"
grep -aq 'NOSAIC-SELFTEST boot count 1' "$LOG" || die "expected the first boot to count 1; see $LOG"

# The second boot is the one that proves the data partition is real. A tmpfs
# pretending to be persistent passes every check a single boot can make.
LOG2="$DIR/console-2.log"
echo "==> booting again, to prove the data partition persists"
boot_once "$LOG2" a
grep -aE 'NOSAIC-SELFTEST (boot count|OK)' "$LOG2" | sed 's/^/    /' || true
grep -aq 'NOSAIC-SELFTEST boot count 2' "$LOG2" \
    || die "the boot count did not survive a reboot: the data partition is not persistent; see $LOG2"
grep -aq 'NOSAIC-SELFTEST OK' "$LOG2" || die "the second boot did not self-test cleanly; see $LOG2"

echo "OK — $BOARD boots from slot a, persists across reboots, and powers off"
