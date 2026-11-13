#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/dts2xml-test.sh -- the board-tree converter, checked against a real tree.
#
# tools/dts2xml.py claims to read the board out of its device tree.  The proof
# is a device tree that was not written for the test: the one inside the
# MikroTik dump, which the vendor's own firmware boots with.  It says the board
# is a "952G_5HacD2HnD": five MDIO PHYs, three lamp lines (one active low) and
# two buttons, a NOR on the QUP and a NAND on the QPIC.
#
#   ./tests/dts2xml-test.sh                     # find the tree in the 32M dump
#   ./tests/dts2xml-test.sh --dump=/path/dump.bin
#   ./tests/dts2xml-test.sh --dtb=/path/board.dtb
#
# Nothing here needs QEMU or libvirt: it checks the translation, not the boot.
set -u
cd "$(dirname "$0")/.." || exit 1

DUMP=$HOME/mikrotik-dump-latest7.17-32m.bin
DTB=
for a in "$@"; do
    case $a in
        --dump=*) DUMP=${a#*=} ;;
        --dtb=*)  DTB=${a#*=} ;;
        -h|--help) sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "dts2xml-test: unknown argument $a" >&2; exit 2 ;;
    esac
done

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/dts2xml.XXXXXX") || exit 1
trap 'rm -rf "$SCRATCH"' EXIT INT TERM
PASSES=0 FAILS=0
pass() { PASSES=$((PASSES + 1)); echo "  ok   $*"; }
fail() { FAILS=$((FAILS + 1)); echo "  FAIL $*"; }

# has <text> <pattern> -- fixed-string containment, because the patterns are
# machine properties full of '=' and ';'
has() { case $1 in *"$2"*) return 0 ;; *) return 1 ;; esac; }

#
# The tree: the first plausible flattened tree in the dump.  "Plausible" is the
# magic plus a total size that fits in the file, which is what skips the magic
# numbers that appear inside compressed data.
#
if [ -z "$DTB" ]; then
    [ -r "$DUMP" ] || { echo "dts2xml-test: no $DUMP (use --dtb=)" >&2; exit 1; }
    DTB=$SCRATCH/board.dtb
    python3 - "$DUMP" "$DTB" <<'PY' || exit 1
import struct, sys
data = open(sys.argv[1], "rb").read()
i = 0
while True:
    i = data.find(b"\xd0\x0d\xfe\xed", i)
    if i < 0:
        sys.exit("no device tree in the dump")
    total = struct.unpack(">I", data[i + 4:i + 8])[0]
    if 4096 < total <= len(data) - i:
        open(sys.argv[2], "wb").write(data[i:i + total])
        break
    i += 4
PY
fi
[ -s "$DTB" ] || { echo "dts2xml-test: no device tree" >&2; exit 1; }

TOOL=./tools/dts2xml.py
[ -x "$TOOL" ] || { echo "dts2xml-test: $TOOL is not executable" >&2; exit 1; }

# ---- what the tree alone says --------------------------------------------
M=$("$TOOL" "$DTB" --print-machine 2>"$SCRATCH/warn")
RC=$?
[ $RC -eq 0 ] || { echo "dts2xml-test: the tool failed"; sed 's/^/    /' "$SCRATCH/warn"; exit 1; }

has "$M" "nor=on" && has "$M" "nand=on" \
    && pass "the tree's two dies become nor=on and nand=on" \
    || fail "the dies were not read from the tree: $M"

has "$M" "eth-ports=0;1;2;3;4" \
    && pass "the five MDIO PHYs of the mdio node become the bus" \
    || fail "the MDIO population is wrong: $M"

has "$M" "leds=user-led:3;power-led:0;usb-power-off:2:al" \
    && pass "lamps keep their labels, pins and polarity" \
    || fail "the lamps were not read from the tree: $M"

has "$M" "keys=" \
    && fail "unbound buttons were turned into keys: $M" \
    || pass "a button with no qcode stays a pin (and is reported)"

grep -q "button button (pin 63)" "$SCRATCH/warn" \
    && pass "the unbound button is named on stderr" \
    || fail "the unbound button was not reported"

# ---- the decisions a tree cannot make ------------------------------------
M=$("$TOOL" "$DTB" --print-machine --key button=esc --key 5=f --nor-size 32M 2>/dev/null)
has "$M" "keys=button:63:al:esc;mode-button:5:al:f" \
    && pass "--key binds a button by name (and by pin)" \
    || fail "button bindings are wrong: $M"
has "$M" "nor-size=32M" \
    && pass "the die size comes from the option" \
    || fail "nor-size did not reach the machine: $M"

M=$("$TOOL" "$DTB" --print-machine --no-nor --no-nand --no-dies 2>/dev/null)
has "$M" "nor=" \
    && fail "--no-dies still described a die: $M" \
    || pass "--no-dies describes a bare SoC"

M=$("$TOOL" "$DTB" --print-machine --no-nor --nand 2>/dev/null)
has "$M" "nor=off" && has "$M" "nand=on" \
    && pass "--no-nor/--nand override the tree" \
    || fail "the die overrides were not applied: $M"

# ---- the two output forms ------------------------------------------------
: > "$SCRATCH/nor.img"
A=$("$TOOL" "$DTB" --print-argv --nor-file "$SCRATCH/nor.img" \
     --image /tmp/none.bin --key button=esc 2>/dev/null)
has "$A" "ipq4019,nor=on" && has "$A" "-drive if=none,id=nor,file=$SCRATCH/nor.img" \
    && pass "argv carries the machine properties and the die backend" \
    || fail "the argv form is wrong: $A"

"$TOOL" "$DTB" --nor-file "$SCRATCH/nor.img" --name dts2xml-test \
    >"$SCRATCH/domain.xml" 2>/dev/null
if command -v xmllint >/dev/null; then
    xmllint --noout "$SCRATCH/domain.xml" 2>"$SCRATCH/xmllint" \
        && pass "the libvirt domain is well-formed" \
        || { fail "the domain XML is not well-formed"; sed 's/^/    /' "$SCRATCH/xmllint"; }
else
    echo "  skip xmllint (not installed)"
fi
has "$(cat "$SCRATCH/domain.xml")" "machine='ipq4019'" \
    && pass "the domain names the machine type" \
    || fail "the domain does not name the machine type"

echo
echo "$PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
