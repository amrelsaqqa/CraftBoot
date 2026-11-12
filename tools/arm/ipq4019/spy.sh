#!/bin/bash
# spy.sh <dump> <cmdfile> [secs]
# Boot the dump, enter APPSBL, install the gdb script in <cmdfile> (which should
# end with 'c') and let it run under a timeout.  Serial log afterwards.
set -u

GDB=${GDB:-/tmp/spy.gsock}
rm -f "$GDB"

# The gdbstub is on a unix socket, never tcp::1234: opening a gdb connection
# makes QEMU call vm_stop(RUN_STATE_PAUSED) and nothing resumes it, so a stray
# probe of a listening localhost port freezes the guest mid-boot.
DUMP=$1; CMDS=$2; SECS=${3:-30}
cd "$(dirname "$0")" || exit 1
SER=/tmp/spy.ser
rm -f "$SER"
env QEMU_IPQ_BOOTDEV_TYPE=${BT:-6} ./build/qemu-system-arm -M ipq4019 \
    -kernel "$DUMP" -display none -serial file:"$SER" -monitor none -S \
    -gdb "unix:$GDB,server=on,wait=off" >/dev/null 2>/tmp/spy.err &
QP=$!
sleep 1
kill -0 "$QP" 2>/dev/null || { echo "qemu died:" >&2; sed 's/^/  /' /tmp/spy.err >&2; exit 1; }

timeout "$SECS" gdb -batch -ex "target remote $GDB" \
    -ex 'b *0x87e80000' -ex 'c' \
    -ex 'set $pc = 0x80000000' \
    -ex 'set $r0 = 0' -ex 'set $r1 = 0' -ex 'set $r2 = 0' -ex 'set $r3 = 0' \
    -x "$CMDS" 2>&1 | grep -v "No executable"
kill -9 $QP 2>/dev/null
wait $QP 2>/dev/null
echo "=== serial:"
sed -n '/SBL1, End/,$p' "$SER" | tail -8
