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
	[ "$status" = scaffold ] || continue
	if git -C "$SRC" show "$REF:asic/fm6000/fm6000_$block.c" > "fm6000_$block.c" 2>/dev/null; then
		n=$((n + 1))
	else
		echo "  missing: fm6000_$block.c"
		rm -f "fm6000_$block.c"
	fi
done < manifest.txt
echo "fetched $n scaffold blocks from $REF"
echo "⚠ none of these are committed, and none of them ship."
