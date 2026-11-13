#!/usr/bin/env python3
"""Dump Breed's kmalloc window just before the 2nd fifo allocation.

    tools/breed-heap-dump.py <port> <out.bin> [addr] [len]
"""
import socket
import sys
import time

GPR = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2',
       't3', 't4', 't5', 't6', 't7', 's0', 's1', 's2', 's3', 's4', 's5',
       's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 's8', 'ra']
PORT = int(sys.argv[1])
OUT = sys.argv[2]
ADDR = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x8fddd600
LEN = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x700
s = socket.create_connection(('127.0.0.1', PORT), timeout=10)


def send(p):
    p = p.encode() if isinstance(p, str) else p
    s.sendall(b'$' + p + b'#' + ('%02x' % (sum(p) & 0xff)).encode())
    return recv()


def recv(t=10):
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


def mem(addr, n):
    out = b''
    while n > 0:
        chunk = min(n, 0x400)
        out += bytes.fromhex(send('m%x,%x' % (addr + len(out), chunk)))
        n -= chunk
    return out


send('qSupported:multiprocess+')
entry = 0x8ffd29d8
send('Z0,%x,4' % entry)
hits = 0
while hits < 2:
    pkt = send('c')
    if not pkt.startswith('T'):
        print('stop', pkt[:40])
        break
    hits += 1
    if hits == 2:
        open(OUT, 'wb').write(mem(ADDR, LEN))
        print('wrote %s (%#x bytes @ %#x)' % (OUT, LEN, ADDR))
    send('z0,%x,4' % entry)
    send('s')
    send('Z0,%x,4' % entry)
send('D')
