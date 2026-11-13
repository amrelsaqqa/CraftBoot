#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Dump the guest's DSA/switch wiring and any driver RX errors.
#   tools/lg-dsa-info.sh
set -u
cd /home/amrelsaqqa/qemu-mt7621 || exit 1
out=run/lg-dsa
PORT=4491
pkill qemu-system-mip >/dev/null 2>&1; sleep 2
rm -rf "$out"; mkdir -p "$out"

MT7621_ETHLOG=1 ./build/qemu-system-mipsel -M mt7621,board=lg-gapm7200 -m 256M \
    -kernel /home/amrelsaqqa/full.bin -display none -monitor none \
    -chardev socket,id=ser0,host=127.0.0.1,port=$PORT,server=on,wait=off,telnet=on,logfile="$out/serial.log" \
    -serial chardev:ser0 \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
    >/dev/null 2>"$out/eth.log" &
qpid=$!

for i in $(seq 1 300); do
    grep -q "br-lan.*link becomes ready" "$out/serial.log" 2>/dev/null && break
    sleep 1
done
sleep 4

python3 tools/guest.py $PORT "$out/serial.log" \
  'ls /proc/device-tree' \
  'find /proc/device-tree -maxdepth 4 -name "port*" | head -20' \
  'ip -d link show lan1' \
  'ip -d link show eth0' \
  'ethtool -S lan1 2>&1 | head -20' \
  'ethtool -S eth0 2>&1 | head -30' \
  'dmesg | grep -i "mtk\|dsa\|switch\|eth" | tail -20' > "$out/info.txt" 2>&1

ping -c 3 -W 1 -I tap0 192.168.33.251 >/dev/null 2>&1
sleep 2
python3 tools/guest.py $PORT "$out/serial.log" \
  'dmesg | tail -15' 'cat /proc/net/dev' > "$out/after.txt" 2>&1

cat "$out/info.txt"
echo "=== after ping ==="
cat "$out/after.txt"
kill $qpid 2>/dev/null
