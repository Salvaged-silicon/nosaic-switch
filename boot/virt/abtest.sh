#!/usr/bin/env bash
#
# Prove the A/B upgrade actually works, both ways.
#
#   boot/virt/abtest.sh <board>
#
# A successful upgrade must commit and a broken one must roll back. Testing
# only the happy path would prove nothing: an implementation that always
# commits passes it, and that implementation has no safety net at all.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
BOARD="${1:?usage: abtest.sh <board>}"

board_field() { sed -n "s/^$2:[[:space:]]*//p" "platform/$BOARD/board.yml" | head -1 | tr -d '"'; }
arch_field()  { sed -n "s/^$2:[[:space:]]*//p" "arch/$1/arch.yml"          | head -1 | tr -d '"'; }
die() { printf '\nFAIL: %s\n' "$*" >&2; exit 1; }

ARCH="$(board_field "$BOARD" arch)"
QEMU="$(arch_field "$ARCH" qemu_system)"
CONSOLE="$(arch_field "$ARCH" qemu_console)"
MACHINE="$(arch_field "$ARCH" qemu_machine)"
CPU="$(arch_field "$ARCH" qemu_cpu)"
DIR="out/images/$BOARD"
N=0

# No nosaic.slot on the command line: the point is that the state machine
# chooses, not us.
boot() {
    N=$((N + 1))
    local log="$DIR/ab-$N.log"
    set +e
    timeout 180 "$QEMU" \
      ${MACHINE:+-machine "$MACHINE"} ${CPU:+-cpu "$CPU"} \
      -nographic -no-reboot -m 1024 \
      -kernel "$DIR/vmlinuz" -initrd "$DIR/initramfs.cpio.gz" \
      -drive "file=$DIR/disk.img,format=raw,if=virtio" \
      -append "console=$CONSOLE panic=5 loglevel=4 nosaic.selftest" \
      </dev/null >"$log" 2>&1
    set -e
    grep -aE 'NOSAIC-BOOT-|NOSAIC-SELFTEST (OK|FAILED|COMMIT|NOCOMMIT)' "$log" | sed 's/^/    /' || true
    LOG="$log"
}

status() { go run ./cmd/nosaic upgrade status "$DIR/disk.img"; }

# Build the image this test is about to make claims on.
#
# The disk was previously whatever a earlier run left behind, and step 1
# asserts it boots slot a -- so the suite passed only while nobody had run it
# twice. The second run started from the slot the first one committed and
# failed its own baseline. A test that depends on leftover state is not
# testing what it says it is.
echo "=== 0. build the image under test ==="
go run ./cmd/nosaic build "$BOARD" >/dev/null

echo "=== 1. baseline: the freshly built disk boots slot a ==="
boot
grep -aq 'NOSAIC-BOOT-SLOT a' "$LOG" || die "expected to boot slot a"
grep -aq 'NOSAIC-SELFTEST OK'  "$LOG" || die "the baseline boot did not self-test cleanly"

echo
echo "=== 2. install a good image into the inactive slot b ==="
go run ./cmd/nosaic upgrade install "$DIR/disk.img" "$DIR/rootfs.sqsh" --slot b
status

echo
echo "=== 3. it boots on trial, confirms itself healthy, and commits ==="
boot
grep -aq 'NOSAIC-BOOT-TRIAL slot b attempt 1' "$LOG" || die "slot b was not tried"
grep -aq 'NOSAIC-BOOT-SLOT b'                 "$LOG" || die "slot b did not boot"
grep -aq 'NOSAIC-SELFTEST COMMIT'             "$LOG" || die "a healthy trial did not commit"
status | grep -q 'active *b' || die "slot b should now be active"

echo
echo "=== 4. install a deliberately broken image into the now-inactive slot a ==="
head -c 4000000 /dev/urandom > "$DIR/broken.img"
go run ./cmd/nosaic upgrade install "$DIR/disk.img" "$DIR/broken.img" --slot a
status

echo
echo "=== 5. the broken slot must roll back, not strand the switch ==="
boot
grep -aq 'NOSAIC-BOOT-ROLLBACK' "$LOG" || die "a slot containing garbage did not roll back"
grep -aq 'NOSAIC-BOOT-SLOT b'   "$LOG" || die "the rollback did not return to the known-good slot"
grep -aq 'NOSAIC-SELFTEST OK'   "$LOG" || die "the rolled-back system is not healthy"
status | grep -q 'trial *none' || die "the failed trial was not cleared"

echo
echo "=== 6. refusing to overwrite the running slot ==="
if go run ./cmd/nosaic upgrade install "$DIR/disk.img" "$DIR/rootfs.sqsh" --slot b 2>/dev/null; then
    die "installing into the active slot was allowed; there would be nothing to roll back to"
fi
echo "    refused, as it must"

echo
echo "OK — a healthy upgrade commits, a broken one rolls back, and the active slot is protected"
