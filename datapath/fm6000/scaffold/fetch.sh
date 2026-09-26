#!/bin/sh
# Populate the scaffold from the EdgeNOS branch. Nothing here is committed;
# see README.md for why.
set -e
SRC=${EDGENOS:-$HOME/projects/edgenos}
REF=${EDGENOS_REF:-remotes/origin/feature/arista-7150-fm6000}
cd "$(dirname "$0")"

[ -d "$SRC/.git" ] || { echo "no EdgeNOS checkout at $SRC (set EDGENOS=)"; exit 1; }

n=0
while read -r status block lines rest; do
	case "$status" in ''|\#*) continue ;; esac
	case "$status" in scaffold|authored) ;; *) continue ;; esac
	if git -C "$SRC" show "$REF:asic/fm6000/fm6000_$block.c" > "fm6000_$block.c" 2>/dev/null; then
		# ⚠ RETARGET TO THE LOCAL BUS. These were written for a chip on
		# the PCI bus: they map the FM6000's own resource0, 32 MB. On a
		# cold 7150S the FM6000 is not on the bus at all -- its registers
		# are reached through the SCD's resource1, which is 16 MB. So
		# every block is repointed on the way in, and mapping 32 MB of a
		# 16 MB BAR would simply fail.
		#
		# Done here rather than by hand so it cannot be half-applied
		# across 29 files, and so a re-fetch is idempotent.
		sed -i 's|/resource0"|/resource1"|; s|32u\*1024\*1024|16u*1024*1024|g; s|32u \* 1024 \* 1024|16u * 1024 * 1024|g' "fm6000_$block.c"
		n=$((n + 1))
	else
		echo "  missing: fm6000_$block.c"
		rm -f "fm6000_$block.c"
	fi
done < manifest.txt
# Headers a few blocks include. fm6000_serdes_ports.h is the board's own
# per-port routing table, which is board data rather than capture.
for h in fm6000_serdes_ports.h fm6000_regs.h fm6000_hw.h; do
	git -C "$SRC" show "$REF:asic/fm6000/$h" > "$h" 2>/dev/null || rm -f "$h"
done

echo "fetched $n scaffold blocks from $REF"
echo "⚠ none of these are committed, and none of them ship."
echo "   retargeted to the SCD local bus: pass the SCD's BDF, not the FM6000's."
