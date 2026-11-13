#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# krun.sh <dump> <appsbl-entry> <marker> <timeout-seconds> <out-prefix> [env...]
#
# Boot a flash dump the usual way -- SBL1 runs first, then the PC is handed to
# APPSBL from the gdbstub -- and let it run until <marker> shows up in the UART
# log or <timeout-seconds> have passed.  QEMU is killed afterwards and the log
# left at <out-prefix>.ser, so one stalled boot does not cost a full timeout.
#
#   krun.sh ~/dump-708ipq-...bin 0x87300000 "Please press Enter" 40 /tmp/ow
#
# QEMU_IPQ_BYPASS=<break-addr>:<entry>   where to stop and where to jump.
#   Every dump so far parks forever inside the vendor TZ (QSEE) blob before it
#   branches to its APPSBL, and the blob needs a protection fabric we do not
#   model, so the harness skips it:
#
#     Caraboot        QEMU_IPQ_BYPASS=0x87e80000:0x87300000
#     MikroTik/kt708  QEMU_IPQ_BYPASS=0xa8d1918:0x80000000
#
#   The break address must be one the loaded firmware does *not* overwrite:
#   breaking at the QSEE entry itself is racy, because SBL1 may load QSEE over
#   the breakpoint after the gdbstub planted it.  An address inside SBL1 (the
#   instruction after the call into QSEE) is stable.
set -u
cd "$(dirname "$0")" || exit 1

DUMP=${1:?dump}
ENTRY=${2:?appsbl entry}
MARKER=${3:?marker}
TIMEOUT=${4:-30}
OUT=${5:-/tmp/krun}
shift 5 2>/dev/null || shift $#

SER=$OUT.ser
rm -f "$SER"

BYPASS=${QEMU_IPQ_BYPASS:-0x87e80000:0x87300000}
BP_ADDR=${BYPASS%%:*}
BP_ENTRY=${BYPASS##*:}

# The gdbstub listens on a unix socket, not tcp::1234.  Anything that opens a
# gdb socket makes QEMU call vm_stop(RUN_STATE_PAUSED) (gdb_chr_event ->
# CHR_EVENT_OPENED) and nothing ever resumes it, so a stray connection from a
# localhost port scanner freezes the guest mid-boot -- it looks like an
# intermittent 20-60x slowdown with the CPU idle in wfi.  A unix socket in a
# private directory is not reachable that way.
if [ "$BP_ENTRY" = "$BYPASS" ]; then
    BP_ENTRY=$ENTRY
fi

GDB=$SER.gdb
rm -f "$GDB"
./build/qemu-system-arm -M ipq4019 -kernel "$DUMP" -display none \
    -serial "file:$SER" -monitor none -S \
    -gdb "unix:$GDB,server=on,wait=off" "$@" \
    >/dev/null 2>&1 &
QP=$!

sleep 1
timeout 60 gdb -batch -ex "target remote $GDB" \
    -ex "b *$BP_ADDR" -ex 'c' \
    -ex "set \$pc = $BP_ENTRY" -ex 'set $r0=0' \
    -ex 'set $cpsr = $cpsr & ~0x20' \
    -ex 'detach' >/dev/null 2>&1

start=$(date +%s)
while :; do
    if grep -qa "$MARKER" "$SER" 2>/dev/null; then
        echo "marker reached after $(( $(date +%s) - start ))s"
        break
    fi
    if [ $(( $(date +%s) - start )) -ge "$TIMEOUT" ]; then
        echo "timeout after ${TIMEOUT}s (no '$MARKER')"
        break
    fi
    sleep 1
done

kill -9 $QP 2>/dev/null
wait $QP 2>/dev/null
echo "ser: $SER"
