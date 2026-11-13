#!/bin/bash
# Boot a flash image with a tap backend and ping the guest from the host.
#
#   tests/mipsel/mt7621/net-ping.sh <image> <guest-ip> [ready-regex] [seconds]
#
# The default ready-regex matches the standard bridge-up message; override it
# when testing firmware with a different console.  TAP (default tap0) must
# exist and carry an address in the guest's subnet.
set -u
cd "$(dirname "$0")/../../.." || exit 1
IMG=${1:?usage: net-ping.sh <image> <guest-ip> [ready-regex] [seconds]}
IP=${2:?}
READY=${3:-'br-lan.*link becomes ready'}
SECS=${4:-120}
TAP=${TAP:-tap0}
BIN=build/mipsel/qemu-system-mipsel
OUT=$(mktemp -d)
trap 'pkill -f "$BIN" 2>/dev/null; rm -rf "$OUT"' EXIT

[ -x "$BIN" ] || { echo "missing $BIN (make TARGET=mipsel build)"; exit 1; }
ip link show "$TAP" >/dev/null 2>&1 || {
    echo "FAIL: tap '$TAP' does not exist; create it or set TAP=<interface>"
    exit 1
}
net=${IP%.*}
if ! ip -4 -o addr show dev "$TAP" | awk '{print $4}' | grep -q "^${net}\."; then
    echo "FAIL: '$TAP' has no address in ${net}.0/24 (guest is $IP);"
    echo "      add one, e.g. sudo ip addr add ${net}.2/24 dev $TAP"
    exit 1
fi

"$BIN" -M mt7621,board=lg-gapm7200 -m 256M -kernel "$IMG" \
    -display none -serial file:"$OUT/serial.log" \
    -netdev tap,id=net0,ifname="$TAP",script=no,downscript=no \
    >/dev/null 2>&1 &
qpid=$!

for _ in $(seq 1 "$SECS"); do
    grep -aq "$READY" "$OUT/serial.log" 2>/dev/null && break
    sleep 1
done
sleep 3

if ! grep -aq "$READY" "$OUT/serial.log" 2>/dev/null; then
    echo "FAIL: guest network never came up ($READY)"
    sed 's/\r$//' "$OUT/serial.log" | tail -10
    kill "$qpid" 2>/dev/null
    exit 1
fi

ping -c 5 -W 2 -I "$TAP" "$IP" > "$OUT/ping.txt" 2>&1
rc=$?
tail -2 "$OUT/ping.txt"
kill "$qpid" 2>/dev/null
[ "$rc" = 0 ] || { echo "FAIL: ping $IP"; exit 1; }
echo "ok: $IP reachable"
