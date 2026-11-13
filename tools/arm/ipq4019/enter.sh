#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# enter.sh <dump> <appsbl-entry> [seconds] [env assignments...]
#
# Boot the dump, stop at the SBL1->QSEE handover and enter APPSBL instead.
# QSEE's TZ bring-up is not modelled yet, and QSEE turns the MMU on within its
# first few instructions, so the PC must be moved *before* QSEE runs -- waiting
# for "SBL1, End" in the log is too late.
set -u

GDB=${GDB:-/tmp/enter.gsock}
rm -f "$GDB"

# The gdbstub is on a unix socket, never tcp::1234: opening a gdb connection
# makes QEMU call vm_stop(RUN_STATE_PAUSED) and nothing resumes it, so a stray
# probe of a listening localhost port freezes the guest mid-boot.
DUMP=$1
ENTRY=$2
SECS=${3:-15}
shift 3 2>/dev/null || shift $#
cd "$(dirname "$0")" || exit 1
SER=/tmp/enter.ser
rm -f "$SER"
env "$@" QEMU_IPQ_BOOTDEV_TYPE=${BT:-6} ./build/qemu-system-arm -M ipq4019 \
    -kernel "$DUMP" \
    -display none -serial file:"$SER" -monitor none -S -gdb "unix:$GDB,server=on,wait=off" \
    >/dev/null 2>/tmp/enter.err &
QP=$!
sleep 1
if ! kill -0 "$QP" 2>/dev/null; then
    # Most often a stale QEMU still owns :1234, and gdb would happily attach to
    # that guest instead of this one.
    echo "enter.sh: QEMU did not stay up:" >&2
    sed 's/^/  /' /tmp/enter.err >&2
    exit 1
fi
timeout 90 gdb -batch -ex "target remote $GDB" \
    -ex 'b *0x87e80000' -ex 'c' \
    -ex "set \$pc = $ENTRY" \
    -ex 'set $r0 = 0' -ex 'set $r1 = 0' -ex 'set $r2 = 0' -ex 'set $r3 = 0' \
    -ex 'detach' >/tmp/enter.gdb 2>&1
sleep "$SECS"
kill -9 $QP 2>/dev/null
wait $QP 2>/dev/null
cat "$SER"
