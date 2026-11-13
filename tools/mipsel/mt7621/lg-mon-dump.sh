#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Did the guest's driver actually consume the RX descriptors we filled?
# Reads the live ring base from the model log, so it follows the guest.
#   tools/lg-mon-dump.sh
set -u
cd /home/amrelsaqqa/qemu-mt7621 || exit 1
out=run/lg-mon
PORT=4492
MON=/tmp/mt-mon.sock
pkill qemu-system-mip >/dev/null 2>&1; sleep 2
rm -rf "$out" "$MON"; mkdir -p "$out"

MT7621_RXDBG=1 MT7621_ETHLOG=1 ./build/qemu-system-mipsel -M mt7621,board=lg-gapm7200 -m 256M \
    -kernel /home/amrelsaqqa/full.bin -display none \
    -chardev socket,id=ser0,host=127.0.0.1,port=$PORT,server=on,wait=off,telnet=on,logfile="$out/serial.log" \
    -serial chardev:ser0 \
    -monitor unix:"$MON",server=on,wait=off \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
    >/dev/null 2>"$out/eth.log" &
qpid=$!

dump() {
    python3 - "$MON" "$1" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1]); s.settimeout(2)
time.sleep(0.3)
s.sendall(("xp /16wx %s\n" % sys.argv[2]).encode())
time.sleep(0.8)
try:
    txt = s.recv(65536).decode('latin1')
except Exception as e:
    txt = 'recv: %s' % e
s.close()
for line in txt.splitlines():
    if line.startswith('0000'):
        print('   ', line)
PY
}

for i in $(seq 1 300); do
    grep -q "br-lan.*link becomes ready" "$out/serial.log" 2>/dev/null && break
    sleep 1
done
sleep 4

BASE=$(grep -a "eth: write 0900 <-" "$out/eth.log" | tail -1 | sed 's/.*<- \([0-9a-f]*\).*/0x\1/')
echo "ring base: ${BASE:-none}"
echo "=== before ping ==="; dump "${BASE:-0x01660000}"

ip neigh flush dev tap0 2>/dev/null
ping -c 3 -W 1 -I tap0 192.168.33.251 >/dev/null 2>&1
sleep 3

BASE=$(grep -a "eth: write 0900 <-" "$out/eth.log" | tail -1 | sed 's/.*<- \([0-9a-f]*\).*/0x\1/')
echo "=== after ping (base $BASE) ==="; dump "${BASE:-0x01660000}"
echo "=== fills ==="; grep -a "eth: fill" "$out/eth.log" | tail -4 | cut -c1-170
echo "=== deliveries ==="; grep -a "rxdeliver" "$out/eth.log" | tail -3 | cut -c1-140
echo "=== guest view ==="
python3 tools/guest.py $PORT "$out/serial.log" \
    "grep -Ei 'ethernet|mtk|13:' /proc/interrupts" \
    'ip -s link show lan1 | tail -3' > "$out/guest.txt" 2>&1
sed 's/\r$//' "$out/guest.txt" | grep -v "^$" | tail -12
kill $qpid 2>/dev/null
