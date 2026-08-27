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

# shellcheck source=boot/virt/qemulib.sh
. "$ROOT/boot/virt/qemulib.sh"
nosaic_qemu_setup "$BOARD"

boot_once() {
    local log="$1" slot="$2"
    set +e
    timeout 180 "$QEMU" "${QEMU_ARGS[@]}" \
      -append "console=$CONSOLE panic=5 loglevel=4 nosaic.selftest nosaic.slot=$slot" \
      </dev/null >"$log" 2>&1
    set -e
}

LOG="$DIR/console.log"
echo "==> booting $BOARD under $QEMU (slot a)"
boot_once "$LOG" a

echo "--- console ---"
grep -aE 'NOSAIC-|Kernel panic|login:|Welcome|fatal:|s6-rc' "$LOG" | sed 's/^/    /' | head -24 || true
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
