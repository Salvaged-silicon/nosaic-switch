# Shared by bootimage.sh, which boots the image as an automated test, and
# vm.sh, which gives a person a console on the same image.
#
# They share this because they must put the image in front of QEMU in exactly
# the same way. If they drift, "it boots in CI" and "it boots on my machine"
# stop being the same claim, and the difference will be found by somebody
# trying NOSaic for the first time.
#
# SPDX-License-Identifier: Apache-2.0

board_field() { sed -n "s/^$2:[[:space:]]*//p" "platform/$1/board.yml" | head -1 | tr -d '"'; }
arch_field()  { sed -n "s/^$2:[[:space:]]*//p" "arch/$1/arch.yml"      | head -1 | tr -d '"'; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# nosaic_qemu_setup <board> resolves the board's emulator and image directory,
# setting QEMU, CONSOLE, DIR and the QEMU_ARGS array.
nosaic_qemu_setup() {
    local board="$1"
    [ -f "platform/$board/board.yml" ] || die "no such board: $board (try: nosaic build)"

    ARCH="$(board_field "$board" arch)"
    QEMU="$(arch_field "$ARCH" qemu_system)"
    CONSOLE="$(arch_field "$ARCH" qemu_console)"
    local machine cpu
    machine="$(arch_field "$ARCH" qemu_machine)"
    cpu="$(arch_field "$ARCH" qemu_cpu)"

    command -v "$QEMU" >/dev/null || die "$QEMU is not installed"

    DIR="out/images/$board"
    local f
    for f in vmlinuz initramfs.cpio.gz disk.img; do
        [ -f "$DIR/$f" ] || die "$DIR/$f missing — run: make image BOARD=$board"
    done

    QEMU_ARGS=()
    [ -n "$machine" ] && QEMU_ARGS+=(-machine "$machine")
    [ -n "$cpu" ]     && QEMU_ARGS+=(-cpu "$cpu")
    QEMU_ARGS+=(
        -nographic -no-reboot -m 1024
        -kernel "$DIR/vmlinuz"
        -initrd "$DIR/initramfs.cpio.gz"
        -drive "file=$DIR/disk.img,format=raw,if=virtio"
    )
}
