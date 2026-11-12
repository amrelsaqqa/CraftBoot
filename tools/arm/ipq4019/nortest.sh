#!/bin/bash
# nortest.sh -- verify the NOR die's program/erase path from inside the guest.
#
# Boots the Caraboot dump into U-Boot, stops the autoboot, and drives U-Boot's
# `sf` commands over the console FIFO: erase a 4 KiB sector, program 16 bytes
# from DRAM, read them back and dump memory.  The point is that this goes
# through U-Boot's own m25p80-style driver, WREN/PP/erase/poll-SR1 and all, so
# it tests the model rather than a shortcut in the harness.
#
#   ./nortest.sh [dump]
set -u
cd "$(dirname "$0")" || exit 1
. ./bypass.sh

DUMP=${1:-$HOME/dump-708ipq-24.10.0-8mb-minimal-extroot-caraboot.bin}
OUT=/tmp/nortest
SER=$OUT.ser
FIFO=$OUT.in
GDB=$OUT.gsock
SECTOR=0x7f0000          # inside the 8 MiB image, an unused gap
PATTERN=0x55
# auto: bypass.sh arms both known handovers and picks the APPSBL entry.
BYPASS=${QEMU_IPQ_BYPASS:-auto}

rm -f "$SER" "$FIFO" "$GDB"
mkfifo "$FIFO"

./build/qemu-system-arm -M ipq4019 -kernel "$DUMP" -display none \
    -serial stdio -monitor none -S \
    -gdb "unix:$GDB,server=on,wait=off" < "$FIFO" > "$SER" 2>&1 &
QP=$!
exec 3>"$FIFO"

sleep 1
if ! bypass_start "$GDB" "$SER" "$OUT.gdb.log" "$BYPASS"; then
    echo "FAIL: the QSEE bypass never armed; see $OUT.gdb.log"
    kill -9 $QP 2>/dev/null; exit 1
fi
trap 'bypass_stop' EXIT INT TERM

# Wait for a string in the log (or until the timeout).
waitfor() {
    local pat=$1 secs=${2:-20} i
    for ((i = 0; i < secs * 10; i++)); do
        grep -qa "$pat" "$SER" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

send() {
    printf '%b' "$1" >&3
}

if ! waitfor "Hit any key to stop autoboot" 30; then
    echo "FAIL: U-Boot never got to the autoboot prompt"
    kill -9 $QP 2>/dev/null; exit 1
fi
send '\r'
if ! waitfor "(IPQ40xx)" 15; then
    echo "FAIL: no U-Boot prompt"
    kill -9 $QP 2>/dev/null; exit 1
fi

# Fill DRAM with the pattern, erase, program, read back, dump.
send 'mw.b 0x82000000 0x55 0x10\r'
sleep 0.5
send "sf probe\r"
sleep 1.5
send "sf erase $SECTOR 0x1000\r"
sleep 1.5
send "sf write 0x82000000 $SECTOR 0x10\r"
sleep 2
send "sf read 0x82100000 $SECTOR 0x10\r"
sleep 1.5
send 'md.b 0x82100000 0x10\r'
sleep 1.5
send "sf probe; sf read 0x82200000 $SECTOR 0x1000\r"
sleep 2
send 'md.b 0x82200000 0x10\r'
sleep 1.5

bypass_stop
kill -9 $QP 2>/dev/null
wait $QP 2>/dev/null

tr '\r' '\n' < "$SER" | grep -v '^$' | sed -n '/(IPQ40xx)/,$p' | tail -40 > "$OUT.tail"
tail -20 "$OUT.tail"
if grep -qE "^82100000: 55 55 55 55 55 55 55 55" "$OUT.tail"; then
    echo "VERDICT program+readback -> PASS"
else
    echo "VERDICT program+readback -> FAIL"
fi
if grep -qE "^82200000: 55 55 55 55 55 55 55 55" "$OUT.tail"; then
    echo "VERDICT erase+readback   -> PASS"
else
    echo "VERDICT erase+readback   -> FAIL"
fi
echo "log: $SER"
