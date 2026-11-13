#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Minimal GDB remote-serial-protocol client for QEMU (MIPS guests).

Used to read guest registers at chosen PCs without a MIPS-capable gdb:

    tools/rsp-probe.py --port 1234 --break 0x8ffd2a20:8 [--break ...]

Each `--break addr[:hits]` stops that many times (default 1) and prints the
named registers from the target description (GPRs, PC, HI, LO).
"""
import argparse
import re
import socket
import sys
import time


class RSP:
    def __init__(self, host, port, timeout=60):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self.buf = b''

    def _recv_byte(self):
        while not self.buf:
            self.buf = self.s.recv(65536)
            if not self.buf:
                raise EOFError('connection closed')
        b, self.buf = self.buf[:1], self.buf[1:]
        return b

    def _packet(self):
        while True:
            c = self._recv_byte()
            if c == b'$':
                break
            if c == b'+':
                continue
        data = b''
        while True:
            c = self._recv_byte()
            if c == b'#':
                break
            data += c
        self._recv_byte()
        self._recv_byte()          # checksum
        self.s.sendall(b'+')
        return data

    def cmd(self, payload, timeout=None):
        if isinstance(payload, str):
            payload = payload.encode()
        csum = sum(payload) & 0xff
        self.s.sendall(b'$' + payload + b'#' + ('%02x' % csum).encode())
        if timeout is not None:
            self.s.settimeout(timeout)
        return self._packet()

    def send_only(self, payload):
        if isinstance(payload, str):
            payload = payload.encode()
        csum = sum(payload) & 0xff
        self.s.sendall(b'$' + payload + b'#' + ('%02x' % csum).encode())

    def close(self):
        try:
            self.cmd('D')
        except Exception:
            pass
        self.s.close()


GPR = ['zero','at','v0','v1','a0','a1','a2','a3',
       't0','t1','t2','t3','t4','t5','t6','t7',
       's0','s1','s2','s3','s4','s5','s6','s7',
       't8','t9','k0','k1','gp','sp','s8','ra']


def mips_regs():
    """QEMU's legacy MIPS32 layout: 32 GPRs, SR, LO, HI, BAD, CAUSE, PC."""
    regs = [(i, name, 32) for i, name in enumerate(GPR)]
    regs += [(32, 'sr', 32), (33, 'lo', 32), (34, 'hi', 32),
             (35, 'bad', 32), (36, 'cause', 32), (37, 'pc', 32)]
    return regs


def read_regs(rsp, regs):
    r = rsp.cmd('g')
    if not r or r[:1] == b'E':
        return {}
    data = bytes.fromhex(r.decode())
    vals = {}
    for num, name, bits in regs:
        if bits != 32:
            continue
        chunk = data[num * 4:num * 4 + 4]
        if len(chunk) < 4:
            continue
        vals[name] = int.from_bytes(chunk, 'little')
    return vals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=1234)
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--break', dest='breaks', action='append', default=[],
                    help='address[:hits]')
    ap.add_argument('--show', default='pc,sp,ra,a0,a1,a2,a3,v0,v1,s0,s1,s2,s3,gp')
    ap.add_argument('--max-stops', type=int, default=40)
    args = ap.parse_args()

    rsp = RSP(args.host, args.port)
    rsp.cmd('qSupported:multiprocess+')
    regs = mips_regs()
    if not regs:
        print('could not read target description', file=sys.stderr)
        return 1

    for spec in args.breaks:
        part = spec.split(':')
        addr = int(part[0], 16)
        for _ in range(int(part[1]) if len(part) > 1 else 1):
            r = rsp.cmd('Z0,%x,4' % addr)
            if r != b'OK':
                print('breakpoint %x rejected: %r' % (addr, r))
                return 1

    want = [n.strip() for n in args.show.split(',')]
    stops = 0
    rsp.s.settimeout(180)
    rsp.send_only('c')
    while stops < args.max_stops:
        stop = rsp._packet()
        text = stop.decode('latin1')
        if not text.startswith(('T', 'S')):
            print('stop: %r' % text)
            break
        vals = read_regs(rsp, regs)
        pc = next((v for k, v in vals.items() if k in ('pc', 'PC')), None)
        shown = ' '.join('%s=%08x' % (k, vals[k]) for k in want if k in vals)
        print('stop %-2d pc=%08x  %s' % (stops, pc or 0, shown))
        stops += 1
        rsp.send_only('c')

    rsp.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
