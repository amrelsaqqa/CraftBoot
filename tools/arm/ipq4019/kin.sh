#!/bin/bash
# kin.sh <dump> <bypass:entry> <marker> <timeout> <out-prefix> [keys] [wait-for] [wait-timeout]
#
# Boot a dump the same way krun.sh does, but with the guest console attached
# to a FIFO so keystrokes can be typed at it.  Needed for OpenWrt's `askfirst`
# getty (it only starts the shell after a newline arrives) and for the
# failsafe menu.
#
#   kin.sh ~/kt708-....bin 0x87e80000:0x80000000 "Please press Enter" 60 /tmp/kt '\r' 'root@OpenWrt' 30
#
# <marker>      wait for this text in the console log before typing
# [keys]        printf %b string sent once the marker appears
# [wait-for]    optional text to wait for after the keys; the exit code is 0
#               only when it shows up
# [wait-timeout] seconds to wait for [wait-for] (default 20)
#
# The FIFO is opened read-write on fd 3, so QEMU never sees EOF; the guest
# console output is captured in <out-prefix>.ser.
set -u
cd "$(dirname "$0")" || exit 1

DUMP=${1:?dump}
BYPASS=${2:?break:entry}
MARKER=${3:?marker}
TIMEOUT=${4:-30}
OUT=${5:-/tmp/kin}
KEYS=${6:-}
WAIT_FOR=${7:-}
WAIT_TIMEOUT=${8:-20}

SER=$OUT.ser
FIFO=$OUT.in
rm -f "$SER" "$FIFO"
mkfifo "$FIFO"

BP_ADDR=${BYPASS%%:*}
BP_ENTRY=${BYPASS##*:}

# The gdbstub listens on a unix socket, not tcp::1234.  Anything that opens a
# gdb socket makes QEMU call vm_stop(RUN_STATE_PAUSED) (gdb_chr_event ->
# CHR_EVENT_OPENED) and nothing ever resumes it, so a stray connection from a
# localhost port scanner freezes the guest mid-boot -- it looks like an
# intermittent 20-60x slowdown with the CPU idle in wfi.  A unix socket in a
# private directory is not reachable that way.

GDB=$OUT.gdb
rm -f "$GDB"
./build/qemu-system-arm -M ipq4019 -kernel "$DUMP" -display none \
    -serial stdio -monitor none -S \
    -gdb "unix:$GDB,server=on,wait=off" \
    < "$FIFO" > "$SER" 2>&1 &
QP=$!

exec 3>"$FIFO"

sleep 1
timeout 60 gdb -batch -ex "target remote $GDB" \
    -ex "b *$BP_ADDR" -ex 'c' \
    -ex "set \$pc = $BP_ENTRY" -ex 'set $r0 = 0' \
    -ex 'set $cpsr = $cpsr & ~0x20' \
    -ex 'detach' >/dev/null 2>&1

start=$(date +%s)
while :; do
    if grep -qa "$MARKER" "$SER" 2>/dev/null; then
        echo "marker '$MARKER' after $(( $(date +%s) - start ))s"
        break
    fi
    if [ $(( $(date +%s) - start )) -ge "$TIMEOUT" ]; then
        echo "timeout after ${TIMEOUT}s (no '$MARKER')"
        kill -9 $QP 2>/dev/null
        exec 3>&-
        rm -f "$FIFO"
        echo "ser: $SER"
        exit 1
    fi
    sleep 1
done

rc=0
if [ -n "$KEYS" ]; then
    printf '%b' "$KEYS" >&3
    echo "keys sent after $(( $(date +%s) - start ))s"
fi

if [ -n "$WAIT_FOR" ]; then
    t0=$(date +%s)
    while :; do
        if grep -qa "$WAIT_FOR" "$SER" 2>/dev/null; then
            echo "matched '$WAIT_FOR' after $(( $(date +%s) - start ))s"
            break
        fi
        if [ $(( $(date +%s) - t0 )) -ge "$WAIT_TIMEOUT" ]; then
            echo "timeout waiting for '$WAIT_FOR'"
            rc=1
            break
        fi
        sleep 1
    done
fi

kill -9 $QP 2>/dev/null
wait $QP 2>/dev/null
exec 3>&-
rm -f "$FIFO"
echo "ser: $SER"
exit $rc
