#!/bin/sh
# Extract Broadcom SDK portmap and SerDes polarity from a Dell S6000-ON
# config.bcm file (e.g. /usr/share/sonic/device/x86_64-dell_s6000_s1220-r0/Force10-S6000/td2-s6000-32x40G.config.bcm).
#
# Usage:
#   ./mkportmap.sh --stdin < td2-s6000-32x40G.config.bcm > portmap.conf
#   QSFP_BREAKOUT=1 ./mkportmap.sh --stdin < td2-s6000-32x40G.config.bcm > portmap.conf
set -eu

QSFP_BREAKOUT=${QSFP_BREAKOUT:-0}

if [ "${1:-}" != "--stdin" ]; then
    echo "usage: $0 --stdin < td2-s6000-32x40G.config.bcm" >&2
    exit 2
fi

awk -v breakout="$QSFP_BREAKOUT" '
BEGIN { next_sub = 33 }
/^[[:space:]]*(#|$)/ { next }
/^portmap_[0-9]+(\.[0-9]+)?=/ {
    sub(/\.[0-9]+=/, "=")
    split($0, kv, "=")
    sub(/^portmap_/, "", kv[1])
    port = kv[1] + 0
    split(kv[2], pv, ":")
    lane = pv[1] + 0
    speed = pv[2] + 0
    if (breakout == 1 && speed == 40) {
        printf "portmap_%d=%d:10\n", port, lane
        for (i = 1; i < 4; i++) {
            subp = next_sub++
            printf "portmap_%d=%d:10\n", subp, lane + i
            printf "tap_swp%d=%d:%d:1500\n", subp, subp, 1000 + subp
        }
    } else {
        printf "portmap_%d=%d:%d\n", port, lane, speed
    }
    next
}
/^xgxs_(rx|tx)_lane_map_/ || /^phy_(xaui_)?(rx|tx)_polarity_flip_/ || /^serdes_(driver_current|pre_driver_current|preemphasis)_/ {
    print $0
}
'
