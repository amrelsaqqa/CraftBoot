#!/bin/bash
# ktrace.sh <dump> <appsbl-entry> <marker> <wait-after> <out-prefix> [env...]
#
# Boot until the UART log shows <marker>, let it run <wait-after> more seconds
# with QEMU_IPQ_TRACE=2 (every unmodelled MMIO access with its PC), then stop.
# Used to see what the kernel touches right before it goes quiet.
set -u

GDB=${GDB:-/tmp/ktrace.gsock}
rm -f "$GDB"

# The gdbstub is on a unix socket, never tcp::1234: opening a gdb connection
# makes QEMU call vm_stop(RUN_STATE_PAUSED) and nothing resumes it, so a stray
# probe of a listening localhost port freezes the guest mid-boot.
cd "$(dirname "$0")" || exit 1

DUMP=${1:?dump}
ENTRY=${2:?appsbl entry}
MARKER=${3:-"Starting kernel"}
AFTER=${4:-10}
OUT=${5:-/tmp/ktrace}
shift 5 2>/dev/null || shift $#

rm -f "$OUT.ser" "$OUT.log"
QEMU_IPQ_TRACE=2 ./build/qemu-system-arm -M ipq4019 -kernel "$DUMP" \
    -display none -serial "file:$OUT.ser" -monitor none \
    -S -gdb "unix:$GDB,server=on,wait=off" -d unimp -D "$OUT.log" "$@" >/dev/null 2>&1 &
QP=$!

sleep 1
timeout 120 gdb -batch -ex "target remote $GDB" \
    -ex 'b *0x87e80000' -ex 'c' \
    -ex "set \$pc = $ENTRY" -ex 'set $r0=0' \
    -ex 'detach' >/dev/null 2>&1

for _ in $(seq 1 300); do
    grep -qa "$MARKER" "$OUT.ser" 2>/dev/null && break
    sleep 1
done
sleep "$AFTER"
kill -9 $QP 2>/dev/null
wait $QP 2>/dev/null
echo "ser: $OUT.ser  log: $OUT.log"
