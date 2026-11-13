#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Boot a flash image and check that the expected lines appear on the console.
#
#   tests/mipsel/mt7621/boot-smoke.sh <image> [seconds]
#
# Expected patterns default to the bootloader and kernel banners; override with
# BOOT_EXPECT (newline-separated regexes).
set -u
cd "$(dirname "$0")/../../.." || exit 1
IMG=${1:?usage: boot-smoke.sh <image> [seconds]}
SECS=${2:-60}
BIN=build/mipsel/qemu-system-mipsel
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

[ -x "$BIN" ] || { echo "missing $BIN (make TARGET=mipsel build)"; exit 1; }

timeout "$SECS" "$BIN" -M mt7621,board=lg-gapm7200 -m 256M -kernel "$IMG" \
    -display none -serial file:"$OUT/serial.log" >/dev/null 2>&1

if [ -n "${BOOT_EXPECT:-}" ]; then
    EXPECT=$BOOT_EXPECT
else
    EXPECT='Boot and Recovery Environment
Linux version
clocksource: GIC'
fi

fail=0
while IFS= read -r pat; do
    [ -z "$pat" ] && continue
    if grep -aq "$pat" "$OUT/serial.log"; then
        echo "ok   : $pat"
    else
        echo "FAIL : $pat"
        fail=1
    fi
done <<< "$EXPECT"

if [ "$fail" != 0 ]; then
    echo "--- serial tail ---"
    sed 's/\r$//' "$OUT/serial.log" | tail -20
fi
exit "$fail"
