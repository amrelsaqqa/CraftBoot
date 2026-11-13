#!/bin/bash
# Step over software breakpoints and dump registers at the allocator sites.
cd /home/amrelsaqqa/qemu-mt7621 || exit 1
pkill qemu-system-mip >/dev/null 2>&1; sleep 1
env ${1:+MT7621_DL=$1} ./build/qemu-system-mipsel -M mt7621,board=lg-gapm7200 -m 256M \
    -kernel /home/amrelsaqqa/full.bin -display none -serial file:/tmp/rsp.log \
    -S -gdb tcp:127.0.0.1:1242 >/dev/null 2>&1 &
sleep 2
python3 - <<'PY'
import socket, time
GPR = ['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5',
       't6','t7','s0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1',
       'gp','sp','s8','ra']
s = socket.create_connection(('127.0.0.1', 1242), timeout=10)
s.settimeout(10)
buf = b''

def send(p):
    p = p.encode() if isinstance(p, str) else p
    s.sendall(b'$' + p + b'#' + ('%02x' % (sum(p) & 0xff)).encode())

def wait(t=10):
    s.settimeout(t)
    data = b''
    while True:
        c = s.recv(65536)
        if not c:
            return ''
        data += c
        i = data.find(b'$')
        if i >= 0:
            j = data.find(b'#', i)
            if j > 0 and len(data) >= j + 3:
                s.sendall(b'+')
                return data[i + 1:j].decode('latin1')

def regs():
    send('g'); d = bytes.fromhex(wait())
    r = {GPR[i]: int.from_bytes(d[i*4:i*4+4], 'little') for i in range(32)}
    r['pc'] = int.from_bytes(d[37*4:37*4+4], 'little')
    return r

def bp(a): send('Z0,%x,4' % a); return wait()
def unbp(a): send('z0,%x,4' % a); return wait()

send('qSupported:multiprocess+'); wait()
sites = [0x8ffd29d8, 0x8ffd2a20, 0x8ffd2a34, 0x8ffd2a3c]
for a in sites:
    print('bp %x -> %s' % (a, bp(a)))

t0 = time.time()
last = None
n = 0
while n < 24 and time.time() - t0 < 20:
    send('c'); pkt = wait(20)
    if not pkt.startswith('T'):
        print('pkt', pkt[:40]); break
    r = regs()
    pc = r['pc']
    print('t=%5.2fs pc=%08x a0=%08x a1=%08x s0=%08x s1=%08x v0=%08x ra=%08x' % (
        time.time()-t0, pc, r['a0'], r['a1'], r['s0'], r['s1'], r['v0'], r['ra']))
    n += 1
    # step over the breakpoint, then re-arm
    unbp(pc); send('s'); wait(10); bp(pc)
s.close()
PY
pkill qemu-system-mip
echo "=== serial tail ==="
sed 's/\r$//' /tmp/rsp.log | tail -4
