#!/bin/bash
# Boot a dump, let it run, then attach and run gdb commands against it.
#   sample.sh <dump> <seconds> [-ex 'cmd']...
set -u

GDB=${GDB:-/tmp/sample.gsock}
rm -f "$GDB"

# The gdbstub is on a unix socket, never tcp::1234: opening a gdb connection
# makes QEMU call vm_stop(RUN_STATE_PAUSED) and nothing resumes it, so a stray
# probe of a listening localhost port freezes the guest mid-boot.
DUMP=$1
SECS=${2:-6}
shift 2
cd "$(dirname "$0")" || exit 1
rm -f /tmp/s.int /tmp/s.ser
QEMU_IPQ_BOOTDEV_TYPE=${BT:-6} ./build/qemu-system-arm -M ipq4019 -kernel "$DUMP" \
    -display none -serial file:/tmp/s.ser -monitor none \
    -gdb "unix:$GDB,server=on,wait=off" -d int,guest_errors -D /tmp/s.int &
QPID=$!
sleep "$SECS"
GARGS=()
while [ $# -gt 0 ]; do
    GARGS+=(-ex "$2")
    shift 2
done
gdb -batch -ex "target remote $GDB" "${GARGS[@]}" 2>/dev/null
kill $QPID 2>/dev/null
wait $QPID 2>/dev/null
