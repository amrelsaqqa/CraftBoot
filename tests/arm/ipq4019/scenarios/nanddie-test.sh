#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/nanddie-test.sh -- the dump <-> die converter, checked against itself.
#
# nanddie.py claims two things that have to hold byte for byte:
#
#   * the spare area is *interleaved* -- one `oob`-byte record after every
#     `page`-byte data page -- never prepended, never appended as a trailer;
#   * nothing is lost in either direction: die -> dump -> die reproduces the
#     input exactly, spare included, when the spare is carried across.
#
# The shapes are also where people get it wrong, so the refusals are part of
# the contract: a data dump is not a die, a die is not a data dump, and a die
# is a whole number of erase blocks.
#
#   ./tests/nanddie-test.sh
#   ./tests/nanddie-test.sh --die=$HOME/nand-test.bin   # also a real 128 MiB die
#
# Nothing here needs QEMU: it checks the converter, not a boot.
set -u
cd "$(dirname "$0")/.." || exit 1

DIE=
for a in "$@"; do
    case $a in
        --die=*) DIE=${a#*=} ;;
        -h|--help) sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "nanddie-test: unknown argument $a" >&2; exit 2 ;;
    esac
done

Q=./nanddie.py
[ -x "$Q" ] || { echo "nanddie-test: $Q is missing or not executable" >&2; exit 1; }
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/nanddie.XXXXXX") || exit 1
trap 'rm -rf "$SCRATCH"' EXIT INT TERM
PASSES=0 FAILS=0
pass() { PASSES=$((PASSES + 1)); echo "  ok   $*"; }
fail() { FAILS=$((FAILS + 1)); echo "  FAIL $*"; }
out() { "$@" >"$SCRATCH/out.log" 2>&1; }

# has <text> <pattern> -- fixed-string containment (the messages carry paths
# and counts, so they are matched by substring rather than by regexp)
has() { case $1 in *"$2"*) return 0 ;; *) return 1 ;; esac; }

#
# A die with known contents: page n holds byte n, so a misaligned page is
# visible as a wrong byte rather than as a plausible one.
#
python3 - "$SCRATCH" <<'PY'
import sys
d = sys.argv[1]
for page, oob, ppblk, name in ((2048, 64, 64, "p2k"), (4096, 128, 128, "p4k")):
    with open("%s/%s-dump.bin" % (d, name), "wb") as f:
        for p in range(ppblk):                 # exactly one erase block
            f.write(bytes([p & 0xff]) * page)
PY

# ---- shape detection -------------------------------------------------------
out "$Q" info "$SCRATCH/p2k-dump.bin"; rc=$?
if [ $rc -eq 0 ] && has "$(cat "$SCRATCH/out.log")" "data-only dump" \
        && ! has "$(cat "$SCRATCH/out.log")" "die image"; then
    pass "info calls a 2048-byte-page image a data-only dump"
else
    fail "info did not recognise the data-only dump"
fi

out "$Q" to-die "$SCRATCH/p2k-dump.bin" "$SCRATCH/p2k-die.bin"
out "$Q" info "$SCRATCH/p2k-die.bin"
if [ $? -eq 0 ] && has "$(cat "$SCRATCH/out.log")" "die image" \
        && has "$(cat "$SCRATCH/out.log")" "1 block of 64 pages"; then
    pass "info calls the interleaved image a die"
else
    fail "info did not recognise the die image: $(tail -1 "$SCRATCH/out.log")"
fi

# ---- dump -> die -----------------------------------------------------------
if [ "$(stat -c %s "$SCRATCH/p2k-die.bin")" = 135168 ]; then
    pass "one 128 KiB block becomes a 135168-byte die (65536 * 2112 shrunk)"
else
    fail "die size is $(stat -c %s "$SCRATCH/p2k-die.bin"), not 135168"
fi

if python3 - "$SCRATCH/p2k-die.bin" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
ok = all(d[p * 2112:p * 2112 + 2048] == bytes([p]) * 2048 for p in range(64))
ok = ok and all(d[p * 2112 + 2048:(p + 1) * 2112] == b"\xff" * 64 for p in range(64))
sys.exit(0 if ok else 1)
PY
then
    pass "the spare is interleaved after each page, filled 0xff"
else
    fail "the die layout is not data-then-64-bytes-of-0xff per page"
fi

# ---- die -> dump, and the round trip --------------------------------------
out "$Q" to-dump "$SCRATCH/p2k-die.bin" "$SCRATCH/p2k-back.bin"
cmp -s "$SCRATCH/p2k-dump.bin" "$SCRATCH/p2k-back.bin" \
    && pass "die -> dump reproduces the original data image" \
    || fail "die -> dump did not reproduce the original"

# The spare is the only thing a plain to-dump throws away, so carrying it
# across has to give the die back exactly -- this is how a chip dump taken
# with OOB survives a trip through a data-only tool chain.
out "$Q" to-dump "$SCRATCH/p2k-die.bin" "$SCRATCH/p2k-data2.bin" \
    --spare "$SCRATCH/p2k-spare.bin"
out "$Q" to-die "$SCRATCH/p2k-data2.bin" "$SCRATCH/p2k-die2.bin" \
    --spare "$SCRATCH/p2k-spare.bin" --verify
cmp -s "$SCRATCH/p2k-die.bin" "$SCRATCH/p2k-die2.bin" \
    && pass "die -> dump + spare -> die is byte identical" \
    || fail "the spare did not round-trip"

if [ "$(stat -c %s "$SCRATCH/p2k-spare.bin")" = 4096 ]; then
    pass "the stripped spare is 64 pages of 64 bytes (4096 bytes)"
else
    fail "spare file is $(stat -c %s "$SCRATCH/p2k-spare.bin") bytes, not 4096"
fi

# ---- a partition that does not start at chip offset 0 ---------------------
out "$Q" to-die "$SCRATCH/p2k-dump.bin" "$SCRATCH/p2k-at.bin" --at-page 64 --verify
if [ "$(stat -c %s "$SCRATCH/p2k-at.bin")" = 270336 ] && python3 - \
        "$SCRATCH/p2k-at.bin" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
sys.exit(0 if d[:135168] == b"\xff" * 135168 and d[135168] == 0 else 1)
PY
then
    pass "--at-page 64 puts a whole erased block in front"
else
    fail "--at-page did not prefix an erased block"
fi

# ---- geometry is not hard-coded -------------------------------------------
out "$Q" to-die --page 4096 --oob 128 --ppblk 128 \
    "$SCRATCH/p4k-dump.bin" "$SCRATCH/p4k-die.bin"
out "$Q" to-dump --page 4096 --oob 128 --ppblk 128 \
    "$SCRATCH/p4k-die.bin" "$SCRATCH/p4k-back.bin"
[ "$(stat -c %s "$SCRATCH/p4k-die.bin")" = 540672 ] \
    && cmp -s "$SCRATCH/p4k-dump.bin" "$SCRATCH/p4k-back.bin" \
    && pass "4096+128 / 128-page blocks work the same way" \
    || fail "the 4096-byte geometry did not round-trip"

# ---- the refusals ----------------------------------------------------------
out "$Q" to-die "$SCRATCH/p2k-die.bin" "$SCRATCH/never.bin"; rc=$?
if [ $rc -ne 0 ] && has "$(cat "$SCRATCH/out.log")" "run \`to-dump\`" \
        && [ ! -e "$SCRATCH/never.bin" ]; then
    pass "to-die on a die is refused and points at to-dump"
else
    fail "to-die accepted a die image (rc=$rc)"
fi

out "$Q" to-dump "$SCRATCH/p2k-dump.bin" "$SCRATCH/never.bin"; rc=$?
if [ $rc -ne 0 ] && has "$(cat "$SCRATCH/out.log")" "run \`to-die\`"; then
    pass "to-dump on a data dump is refused and points at to-die"
else
    fail "to-dump accepted a data-only dump (rc=$rc)"
fi

python3 -c "open('$SCRATCH/half.bin','wb').write(b'\x00' * 2048 * 32)"
out "$Q" to-die "$SCRATCH/half.bin" "$SCRATCH/never.bin"; rc=$?
if [ $rc -ne 0 ] && has "$(cat "$SCRATCH/out.log")" "whole erase block" \
        && [ ! -e "$SCRATCH/never.bin" ]; then
    pass "half a block is refused, and no partial die is left behind"
else
    fail "a partial block was written as a die (rc=$rc)"
fi

out "$Q" to-die "$SCRATCH/half.bin" "$SCRATCH/half-die.bin" --force
[ "$(stat -c %s "$SCRATCH/half-die.bin" 2>/dev/null)" = 67584 ] \
    && pass "--force writes the partial block anyway" \
    || fail "--force did not write the partial block"

printf 'x' >>"$SCRATCH/p2k-dump.bin"
out "$Q" to-die "$SCRATCH/p2k-dump.bin" "$SCRATCH/never.bin"; rc=$?
[ $rc -ne 0 ] && [ ! -e "$SCRATCH/never.bin" ] \
    && pass "a dump that ends mid-page is refused" \
    || fail "a truncated page slipped through (rc=$rc)"

# ---- a real die, when there is one ----------------------------------------
if [ -n "$DIE" ]; then
    [ -r "$DIE" ] || { echo "nanddie-test: cannot read $DIE" >&2; exit 1; }
    out "$Q" to-dump "$DIE" "$SCRATCH/real-dump.bin" --spare "$SCRATCH/real-spare.bin"
    out "$Q" to-die "$SCRATCH/real-dump.bin" "$SCRATCH/real-die.bin" \
        --spare "$SCRATCH/real-spare.bin" --verify
    cmp -s "$DIE" "$SCRATCH/real-die.bin" \
        && pass "$(basename "$DIE"): 128 MiB die survives die -> dump -> die" \
        || fail "$(basename "$DIE"): the real die did not round-trip"
fi

echo
if [ "$FAILS" = 0 ]; then
    echo "nanddie: $PASSES passed, 0 failed"
    exit 0
fi
echo "nanddie: $PASSES passed, $FAILS FAILED"
exit 1
