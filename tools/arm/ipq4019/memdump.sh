#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# memdump.sh <dump> <seconds> <addr> <len> <out.bin> [entry]
# Boot like enter.sh, let RouterBOOT run, then attach with gdb and dump RAM.
set -u

GDB=${GDB:-/tmp/memdump.gsock}
rm -f "$GDB"

# The gdbstub is on a unix socket, never tcp::1234: opening a gdb connection
# makes QEMU call vm_stop(RUN_STATE_PAUSED) and nothing resumes it, so a stray
# probe of a listening localhost port freezes the guest mid-boot.
DUMP=$1; SECS=$2; ADDR=$3; LEN=$4; OUT=$5; ENTRY=${6:-0x80000000}
cd "$(dirname "$0")" || exit 1
SER=/tmp/memdump.ser
rm -f "$SER"
env QEMU_IPQ_BOOTDEV_TYPE=${BT:-6} ./build/qemu-system-arm -M ipq4019 \
    -kernel "$DUMP" -display none -serial file:"$SER" -monitor none -S \
    -gdb "unix:$GDB,server=on,wait=off" >/dev/null 2>/tmp/memdump.err &
QP=$!
sleep 1
kill -0 "$QP" 2>/dev/null || { echo "qemu died:" >&2; sed 's/^/  /' /tmp/memdump.err >&2; exit 1; }

timeout 90 gdb -batch -ex "target remote $GDB" \
    -ex 'b *0x87e80000' -ex 'c' \
    -ex "set \$pc = $ENTRY" \
    -ex 'set $r0 = 0' -ex 'set $r1 = 0' -ex 'set $r2 = 0' -ex 'set $r3 = 0' \
    -ex 'detach' >/dev/null 2>&1

sleep "$SECS"

END=$(python3 -c "print(hex($ADDR + $LEN))")
timeout 90 gdb -batch -ex "target remote $GDB" \
    -ex "dump binary memory $OUT $ADDR $END" \
    -ex 'info registers sp lr pc cpsr' \
    -ex "x/24xw $ADDR" \
    -ex 'detach' 2>&1 | tail -30
kill -9 $QP 2>/dev/null
wait $QP 2>/dev/null
echo "=== serial tail:"
sed -n '/SBL1, End/,$p' "$SER" | tail -6
