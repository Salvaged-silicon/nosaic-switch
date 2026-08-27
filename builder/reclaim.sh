#!/bin/sh
#
# Remove build trees that may contain root-owned files.
#
#   NOSAIC_RUN="<docker run ...>" builder/reclaim.sh <dir>...
#
# The build runs as root inside the container, so the trees it leaves behind
# are full of root-owned files. The host removes almost all of it anyway --
# permission to unlink comes from the parent directory, not from owning the
# file -- but a directory the container made non-writable defeats it, because
# the host cannot chmod a file it does not own. A measured run left 1.3 GB of a
# 14 GB tree behind that way, and reported success.
#
# So the host is tried first and the container is the fallback. That order
# matters: on a host using Docker's vfs storage driver a single container start
# costs minutes, which would make `make clean` unusable if it were the default.
#
# SPDX-License-Identifier: Apache-2.0
set -eu

[ $# -gt 0 ] || exit 0

for d in "$@"; do
    [ -e "$d" ] || continue
    chmod -R u+w "$d" 2>/dev/null || true
    rm -rf "$d" 2>/dev/null || true
done

left=""
for d in "$@"; do
    [ -e "$d" ] && left="$left $d"
done
[ -n "$left" ] || exit 0

if [ -z "${NOSAIC_RUN:-}" ]; then
    echo "could not remove:$left" >&2
    echo "they are owned by root, from a container build; re-run without NATIVE=1" >&2
    exit 1
fi

echo "removing root-owned leftovers in the container:$left"
# shellcheck disable=SC2086
$NOSAIC_RUN sh -c "chmod -R u+w $left 2>/dev/null; rm -rf $left"
