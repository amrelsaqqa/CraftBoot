#!/bin/bash
# ubdbg.sh <dump> <appsbl-entry> [seconds] [-ex gdbcmd]...
#
# Boot the dump, divert the SBL1->QSEE handover into APPSBL (see enter.sh),
# let it run, then attach gdb and run the given commands against the
# (usually hung) guest.
set -u

GDB=${GDB:-/tmp/ubdbg.gsock}
rm -f "$GDB"

# The gdbstub is on a unix socket, never tcp::1234: opening a gdb connection
# makes QEMU call vm_stop(RUN_STATE_PAUSED) and nothing resumes it, so a stray
# probe of a listening localhost port freezes the guest mid-boot.
DUMP=$1
ENTRY=$2
SECS=$3
shift 3
cd "$(dirname "$0")" || exit 1
rm -f /tmp/ub.ser
QEMU_IPQ_BOOTDEV_TYPE=${BT:-6} ./build/qemu-system-arm -M ipq4019 \
    -kernel "$DUMP" \
    -display none -serial file:/tmp/ub.ser -monitor none -S -gdb "unix:$GDB,server=on,wait=off" \
    >/dev/null 2>&1 &
QP=$!
sleep 1
timeout 90 gdb -batch -ex "target remote $GDB" \
    -ex 'b *0x87e80000' -ex 'c' \
    -ex "set \$pc = $ENTRY" \
    -ex 'set $r0 = 0' -ex 'set $r1 = 0' -ex 'set $r2 = 0' -ex 'set $r3 = 0' \
    -ex 'detach' >/dev/null 2>&1
sleep "$SECS"
timeout "${GT:-60}" gdb -batch -ex "target remote $GDB" "$@" 2>&1 | grep -v "^ipq4019"
kill -9 $QP 2>/dev/null
wait $QP 2>/dev/null
