#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# hang.sh <dump> <seconds> <gdb-cmdfile> [entry]
# Boot like enter.sh, let RouterBOOT run (and hang), then attach with gdb and
# run whatever is in the command file.  Prints the serial log afterwards.
set -u

GDB=${GDB:-/tmp/hang.gsock}
rm -f "$GDB"

# The gdbstub is on a unix socket, never tcp::1234: opening a gdb connection
# makes QEMU call vm_stop(RUN_STATE_PAUSED) and nothing resumes it, so a stray
# probe of a listening localhost port freezes the guest mid-boot.
DUMP=$1; SECS=$2; CMDS=$3; ENTRY=${4:-0x80000000}
cd "$(dirname "$0")" || exit 1
SER=/tmp/hang.ser
rm -f "$SER"
env QEMU_IPQ_BOOTDEV_TYPE=${BT:-6} ./build/qemu-system-arm -M ipq4019 \
    -kernel "$DUMP" -display none -serial file:"$SER" -monitor none -S \
    -gdb "unix:$GDB,server=on,wait=off" >/dev/null 2>/tmp/hang.err &
QP=$!
sleep 1
kill -0 "$QP" 2>/dev/null || { echo "qemu died:" >&2; sed 's/^/  /' /tmp/hang.err >&2; exit 1; }

timeout 90 gdb -batch -ex "target remote $GDB" \
    -ex 'b *0x87e80000' -ex 'c' \
    -ex "set \$pc = $ENTRY" \
    -ex 'set $r0 = 0' -ex 'set $r1 = 0' -ex 'set $r2 = 0' -ex 'set $r3 = 0' \
    -ex 'detach' >/dev/null 2>&1

sleep "$SECS"

timeout 90 gdb -batch -ex "target remote $GDB" -x "$CMDS" -ex 'detach' 2>&1 | grep -v "No executable"
kill -9 $QP 2>/dev/null
wait $QP 2>/dev/null
echo "=== serial:"
sed -n '/SBL1, End/,$p' "$SER" | tail -12
