#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Find where Breed's decompressed payload lives in RAM, by reproducing the
# Dl=4 failure and dumping the two candidate windows.
#   tools/breed-dump.sh
set -u
cd /home/amrelsaqqa/qemu-mt7621 || exit 1
out=run/breed-dump
MON=/tmp/breed-mon.sock
pkill qemu-system-mip >/dev/null 2>&1; sleep 2
rm -rf "$out" "$MON"; mkdir -p "$out"

MT7621_DL=4 ./build/qemu-system-mipsel -M mt7621,board=lg-gapm7200 -m 256M \
    -kernel /home/amrelsaqqa/full.bin -display none \
    -serial file:"$out/serial.log" \
    -monitor unix:"$MON",server=on,wait=off \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
    >/dev/null 2>"$out/qemu.log" &
qpid=$!

for i in $(seq 1 60); do
    grep -q "unable to allocate fifos" "$out/serial.log" 2>/dev/null && break
    sleep 0.5
done
sleep 1

python3 - "$MON" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1]); s.settimeout(60)
time.sleep(0.3)
for cmd in ('pmemsave 0x0ffc0000 0x400000 /tmp/breed-hi.bin',
            'pmemsave 0x01000000 0x400000 /tmp/breed-lo.bin',
            'pmemsave 0x00000000 0x400000 /tmp/breed-zero.bin'):
    s.sendall((cmd + "\n").encode())
    time.sleep(2)
    try:
        print(s.recv(4096).decode('latin1').strip()[-160:])
    except Exception as e:
        print('recv:', e)
s.close()
PY

echo "=== serial tail ==="
sed 's/\r$//' "$out/serial.log" | tail -6
for f in /tmp/breed-hi.bin /tmp/breed-lo.bin /tmp/breed-zero.bin; do
    [ -f "$f" ] || continue
    n=$(grep -ac "unable to allocate fifos" "$f" 2>/dev/null)
    m=$(grep -ac "Breed MT7621" "$f" 2>/dev/null)
    echo "$f fifos=$n breed-hdr=$m"
done
kill $qpid 2>/dev/null
