#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Quick SBL1-only regression test: boots a dump until SBL1 ends or errors and
# prints the last few UART lines plus a PASS/FAIL verdict.
#
#   ./sbltest.sh                  # default dump (OpenWrt/Caraboot)
#   DUMP=~/mikrotik-...bin ./sbltest.sh
#
# Anything else in the environment (QEMU_IPQ_*) is passed through to QEMU, so
# knobs can be bisected without editing this script.
set -u
cd "$(dirname "$0")"
DUMP=${DUMP:-$HOME/dump-708ipq-24.10.0-8mb-minimal-extroot-caraboot.bin}
SER=/tmp/sbltest.ser
rm -f "$SER"

./build/qemu-system-arm -M ipq4019 -kernel "$DUMP" -display none \
    -serial file:"$SER" -monitor none >/dev/null 2>&1 &
QP=$!
for _ in $(seq 1 200); do
    grep -qaE "SBL1, End|Boot error" "$SER" 2>/dev/null && break
    kill -0 $QP 2>/dev/null || break
    sleep 0.25
done
kill -9 $QP 2>/dev/null; wait $QP 2>/dev/null

tr '\r' '\n' < "$SER" | grep -v '^$' | tail -4
if grep -qa "SBL1, End" "$SER"; then
    echo "VERDICT $(basename "$DUMP") -> PASS"
else
    echo "VERDICT $(basename "$DUMP") -> FAIL"
fi
