#!/bin/bash
# Regenerate the root repo's QEMU artifacts from the qemu working tree.
#
# The qemu/ checkout is the single source of truth for the emulator changes
# (branch mt7621-model).  The root repo keeps two generated copies so it stays
# self-contained and reproducible without carrying a full QEMU tree:
#
#   hw-mips-mt7621.c    the machine model (qemu/hw/mips/mt7621.c)
#   qemu-mt7621.patch   every other QEMU-side change, diffed against the
#                       upstream base commit (f8ed816)
#
# Run this after changing anything under qemu/, then commit both repos.
#   tools/sync-qemu.sh [--check]
set -eu
cd "$(dirname "$0")/.." || exit 1

BASE=f8ed816          # upstream QEMU commit the branch is based on
CHECK=${1:-}

if ! git -C qemu rev-parse --verify "$BASE" >/dev/null 2>&1; then
    echo "qemu: base commit $BASE not found" >&2
    exit 1
fi

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
git -C qemu diff "$BASE"..HEAD -- . ':(exclude)hw/mips/mt7621.c' \
    ':(exclude)build' > "$tmp"

if [ "$CHECK" = "--check" ]; then
    rc=0
    if ! cmp -s "$tmp" qemu-mt7621.patch; then
        echo "qemu-mt7621.patch is stale (run tools/sync-qemu.sh)"
        rc=1
    fi
    if ! cmp -s qemu/hw/mips/mt7621.c hw-mips-mt7621.c; then
        echo "hw-mips-mt7621.c is stale (run tools/sync-qemu.sh)"
        rc=1
    fi
    exit $rc
fi

cp "$tmp" qemu-mt7621.patch
cp qemu/hw/mips/mt7621.c hw-mips-mt7621.c

printf 'synced: qemu-mt7621.patch (%d files), hw-mips-mt7621.c (%s lines)\n' \
    "$(grep -c '^diff --git' qemu-mt7621.patch)" \
    "$(wc -l < hw-mips-mt7621.c)"

# The patch must still apply to a pristine upstream tree.
wt=$(mktemp -d)
git -C qemu worktree add -q --detach "$wt" "$BASE"
if ! git -C "$wt" apply --check "$PWD/qemu-mt7621.patch"; then
    echo "warning: qemu-mt7621.patch does not apply to $BASE" >&2
fi
git -C qemu worktree remove --force "$wt"
