#!/bin/sh
# Start the RB750Gr3 MT7621 guest for netinstall work.
#
#   tools/ni-vm.sh                 telnet console on 127.0.0.1:4444, logs in run/
#   tools/ni-vm.sh -t 0            no telnet, console only in run/serial.log
#   tools/ni-vm.sh -q              quiet: only flash save, no trace knobs
#   tools/ni-vm.sh -f img.bin -o /somewhere
#
# Artifacts (default run/):
#   serial.log   guest console (always, even with telnet)
#   qemu.err     host stderr: SPI/NI/eth traces from MT7621_* knobs
#   flash.bin    live 16 MiB NOR image, rewritten every 2 s
#   monitor.sock HMP socket:  printf 'info status\n' | socat - UNIX-CONNECT:run/monitor.sock
#
# The console is a telnet chardev so the guest can be driven interactively
# while everything is still logged to serial.log:
#   telnet 127.0.0.1 4444
set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-mipsel}
IMG=${IMG:-$REPO/rb750g3-dump-7.17.bin}
OUT=${OUT:-$REPO/run}
PORT=4444
QUIET=

while [ $# -gt 0 ]; do
    case $1 in
    -t) PORT=$2; shift 2 ;;
    -f) IMG=$2; shift 2 ;;
    -o) OUT=$2; shift 2 ;;
    -q) QUIET=1; shift ;;
    -v) VERBOSE=1; shift ;;
    -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "ni-vm.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

[ -x "$QEMU" ] || { echo "ni-vm.sh: no emulator at $QEMU" >&2; exit 1; }
[ -r "$IMG" ] || { echo "ni-vm.sh: no flash image at $IMG" >&2; exit 1; }
mkdir -p "$OUT"

# Knobs that matter for netinstall (keep in sync with the hand-run command):
#   CMGCR/CMGIC  the 5.6.3 netboot kernel needs the CM + GIC advertised
#   RX_LSO       laptop runs it; the guest's handshake path is sensitive to it
#   FLASH_SAVE   the 4 MiB window cannot see the rest of the chip
# Trace knobs are host-side only (they never change guest behaviour).
KNOBS="MT7621_CMGCR=1 MT7621_CMGIC=1 MT7621_RX_LSO=1 MT7621_ETH_ANYLINK=1 MT7621_FLASH_SAVE=$OUT/flash.bin"
# Defaults mirror the hand-run command: RECVLOG + NILOG.  -q drops them, -v
# adds the very chatty SPI traces (use only when debugging flash access).
[ -z "$QUIET" ] && KNOBS="$KNOBS MT7621_RECVLOG=1 MT7621_NILOG=1"
[ -n "$VERBOSE" ] && KNOBS="$KNOBS MT7621_SPILOG=1 MT7621_WRLOG=1"

rm -f "$OUT/monitor.sock" "$OUT/serial.log" "$OUT/qemu.err" "$OUT/flash.bin"

if [ "$PORT" = 0 ]; then
    SERIAL="-serial file:$OUT/serial.log"
else
    SERIAL="-chardev socket,id=ser0,host=127.0.0.1,port=$PORT,server=on,wait=off,telnet=on,logfile=$OUT/serial.log -serial chardev:ser0"
fi

# shellcheck disable=SC2086
env $KNOBS "$QEMU" \
    -M mt7621,board=rb750gr3 -m 256M \
    -kernel "$IMG" \
    -display none \
    -monitor "unix:$OUT/monitor.sock,server=on,wait=off" \
    $SERIAL \
    -netdev tap,id=net0,ifname=ipqtap0,script=no,downscript=no \
    > "$OUT/qemu.out" 2> "$OUT/qemu.err" &

echo $! > "$OUT/qemu.pid"
echo "ni-vm.sh: qemu pid $(cat "$OUT/qemu.pid")"
[ "$PORT" = 0 ] || echo "ni-vm.sh: telnet 127.0.0.1 $PORT for the guest console"
echo "ni-vm.sh: serial  $OUT/serial.log"
echo "ni-vm.sh: flash   $OUT/flash.bin"
echo "ni-vm.sh: monitor $OUT/monitor.sock"
