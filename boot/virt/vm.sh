#!/usr/bin/env bash
#
# Run a composed NOSaic image as an interactive VM.
#
#   boot/virt/vm.sh <board> [slot]
#
# This is the same image, the same kernel and the same boot path the automated
# test uses -- the QEMU invocation is shared, in qemulib.sh, so the two cannot
# drift. The differences are the ones a person needs: no self-test, no timeout,
# and the console attached to this terminal instead of a log file.
#
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
BOARD="${1:?usage: vm.sh <board> [slot]}"
SLOT="${2:-a}"

# shellcheck source=boot/virt/qemulib.sh
. "$ROOT/boot/virt/qemulib.sh"
nosaic_qemu_setup "$BOARD"

cat <<BANNER

  NOSaic — $BOARD, slot $SLOT

  Log in as 'admin'. There is no password: NOSaic ships without one, so that
  no image is ever reachable with a credential its author already knows. Set
  one with 'passwd'; it lands on the data partition and survives upgrades and
  rollbacks. Until it is set, this account works on the console only and SSH
  refuses password authentication.

  To leave QEMU:  Ctrl-a x        To power off cleanly:  poweroff

BANNER

# No -no-reboot: a person rebooting the VM should get a reboot, not an exit.
# The self-test flag is deliberately absent -- it runs checks and powers off,
# which is right for CI and useless for a session.
# Substitution would blank the element rather than drop it, and QEMU rejects
# an empty argument, so filter into a new array.
ARGS=()
for a in "${QEMU_ARGS[@]}"; do
    [ "$a" = "-no-reboot" ] && continue
    ARGS+=("$a")
done

exec "$QEMU" "${ARGS[@]}" \
  -append "console=$CONSOLE panic=5 loglevel=4 nosaic.slot=$SLOT"
