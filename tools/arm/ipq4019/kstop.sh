#!/bin/bash
# kstop.sh <dump> <appsbl-entry> <marker> <wait-after> <out-prefix> [env...]
#
# Start QEMU stopped, let SBL1 run, hand the PC to APPSBL (same trick as
# enter.sh), then wait until the UART log contains <marker> plus <wait-after>
# seconds and finally stop the guest and dump
#
#   <out-prefix>.regs   CPU registers at the stop point
#   <out-prefix>.ram    first 32 MiB of DRAM (pmemsave, physical addresses)
#
# pmemsave is used instead of gdb because once the kernel turns the MMU on,
# gdb's memory reads are translated through the current page tables and a
# physical address like 0x80000000 no longer resolves.
set -u

GDB=${GDB:-/tmp/kstop.gsock}
rm -f "$GDB"

# The gdbstub is on a unix socket, never tcp::1234: opening a gdb connection
# makes QEMU call vm_stop(RUN_STATE_PAUSED) and nothing resumes it, so a stray
# probe of a listening localhost port freezes the guest mid-boot.
cd "$(dirname "$0")" || exit 1

DUMP=${1:?dump}
ENTRY=${2:?appsbl entry}
MARKER=${3:-"Starting kernel"}
AFTER=${4:-8}
OUT=${5:-/tmp/kstop}
shift 5 2>/dev/null || shift $#

SER=$OUT.ser
MON=$OUT.mon
rm -f "$SER" "$MON" "$OUT.regs" "$OUT.ram"

./build/qemu-system-arm -M ipq4019 -kernel "$DUMP" -display none \
    -serial "file:$SER" -monitor "unix:$MON,server,nowait" \
    -S -gdb "unix:$GDB,server=on,wait=off" "$@" >/dev/null 2>&1 &
QP=$!

sleep 1
timeout 120 gdb -batch -ex "target remote $GDB" \
    -ex 'b *0x87e80000' -ex 'c' \
    -ex "set \$pc = $ENTRY" -ex 'set $r0=0' \
    -ex 'detach' >/dev/null 2>&1

# wait for the marker, then let it run on a little
for i in $(seq 1 600); do
    if grep -qa "$MARKER" "$SER" 2>/dev/null; then break; fi
    sleep 1
done
sleep "$AFTER"

python3 - "$MON" "$OUT" <<'PY'
import socket, sys, time
mon, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(mon)
time.sleep(0.3)
try:
    s.recv(65536)
except OSError:
    pass
def cmd(c, wait=1.0):
    s.send((c + "\n").encode())
    time.sleep(wait)
    try:
        return s.recv(1 << 20).decode(errors="replace")
    except OSError:
        return ""
regs = cmd("info registers")
open(out + ".regs", "w").write(regs)
cmd('pmemsave 0x80000000 0x2000000 "%s.ram"' % out, 3.0)
PY

kill -9 $QP 2>/dev/null
wait $QP 2>/dev/null
echo "ser: $SER  regs: $OUT.regs  ram: $OUT.ram"
