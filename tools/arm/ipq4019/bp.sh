#!/bin/bash
# Start QEMU frozen, attach gdb, run the given -ex commands (typically
# "b *ADDR" then "c").  Usage: bp.sh <dump> -ex 'b *0x...' -ex 'c' -ex 'info registers'
set -u

GDB=${GDB:-/tmp/bp.gsock}
rm -f "$GDB"

# The gdbstub is on a unix socket, never tcp::1234: opening a gdb connection
# makes QEMU call vm_stop(RUN_STATE_PAUSED) and nothing resumes it, so a stray
# probe of a listening localhost port freezes the guest mid-boot.
DUMP=$1
shift
cd "$(dirname "$0")" || exit 1
QEMU_IPQ_BOOTDEV_TYPE=${BT:-6} ./build/qemu-system-arm -M ipq4019 -kernel "$DUMP" \
    -display none -serial file:/tmp/bp.ser -monitor none -S -gdb "unix:$GDB,server=on,wait=off" \
    >/dev/null 2>&1 &
QP=$!
sleep 1
timeout "${GT:-120}" gdb -batch -ex "target remote $GDB" "$@" 2>&1 | grep -v "^ipq4019"
kill $QP 2>/dev/null
wait $QP 2>/dev/null
