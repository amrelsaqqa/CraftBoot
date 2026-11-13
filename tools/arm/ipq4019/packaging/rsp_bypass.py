#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""rsp_bypass.py -- re-arm the QSEE redirect without needing gdb.

The image's SBL1 hands over to QSEE, which needs the QCOM protection fabric
this machine does not model, so the harness stops the CPU at the handover and
redirects it to APPSBL with r0 = 0 ("monitor returned OK").  bypass.sh does
that with a resident gdb agent; this is the same agent speaking the gdb remote
serial protocol directly.

Why not just ship gdb:

  * the redirect has to survive a guest `reboot` (qcom,pshold -> reset -> SBL1
    again), so the breakpoint has to stay armed for the whole session;
  * the packaged builds must run on Windows, where a gdb that understands ARM
    is not something a user should have to find;
  * the protocol needed is four commands -- Z1 (hardware breakpoint), c
    (continue), p/P (read/write a register) -- and writing them here makes the
    dependency a Python 3 interpreter that the image already has.

A *hardware* breakpoint (Z1) is used on purpose: a software breakpoint is a
breakpoint instruction, and SBL1 reloads its own image on every boot, so the breakpoint
byte would vanish and the redirect would never fire again.

  python3 rsp_bypass.py unix:/tmp/run.gsock 0xa8d1918:0x80000000
  python3 rsp_bypass.py 127.0.0.1:1234 0x87e80000:0x87300000

Prints one "bypass: ..." line per hit; the launcher waits for the first one to
know the redirect is armed.
"""

import os
import socket
import sys
import time

DEBUG = bool(os.environ.get("RSP_DEBUG"))


def dbg(msg):
    if DEBUG:
        sys.stderr.write("rsp_bypass: %s\n" % msg)
        sys.stderr.flush()

# ARM (gdb numbering): r0..r15 = 0..15, cpsr = 25.  The protocol spells register
# numbers in hex, so r0 is p0, pc is pf and cpsr is p19.
REG_R0 = 0
REG_PC = 15
REG_CPSR = 25
CPSR_T = 0x20            # Thumb state: the redirect lands in ARM state


class Remote:
    def __init__(self, target, timeout=30.0):
        if target.startswith("unix:"):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(timeout)
            s.connect(target[5:])
        else:
            host, _, port = target.rpartition(":")
            s = socket.create_connection((host or "127.0.0.1", int(port)),
                                         timeout=timeout)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1) \
            if s.family == socket.AF_INET else None
        s.settimeout(None)
        self.s = s
        self.buf = b""

    def _fill(self):
        d = self.s.recv(4096)
        if not d:
            raise EOFError("the remote closed the connection")
        self.buf += d

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        pkt = b"$" + data + b"#" + ("%02x" % (sum(data) & 0xff)).encode()
        dbg("-> %r" % data)
        self.s.sendall(pkt)
        # Wait for the acknowledgement; QEMU answers every packet we send.
        while True:
            if not self.buf:
                self._fill()
            c, self.buf = self.buf[:1], self.buf[1:]
            if c == b"+":
                return
            if c == b"-":
                self.s.sendall(pkt)
                continue

    def recv(self):
        """Return the payload of the next packet, skipping acks and notices."""
        while True:
            while not self.buf:
                self._fill()
            if self.buf[:1] in (b"+", b"-"):
                self.buf = self.buf[1:]
                continue
            if self.buf[:1] != b"$":
                self.buf = self.buf[1:]
                continue
            end = self.buf.find(b"#")
            if end < 0 or len(self.buf) < end + 3:
                self._fill()
                continue
            payload = self.buf[1:end]
            self.buf = self.buf[end + 3:]
            self.s.sendall(b"+")
            dbg("<- %r" % payload)
            # 'O' packets are console output from the remote; drop them.  The
            # one reply that starts with the same letter is the breakpoint's
            # "OK", so it has to be exempted explicitly -- filtering on the
            # first byte alone swallowed it and the agent waited for ever.
            if payload[:1] == b"O" and payload != b"OK":
                continue
            return payload

    def cmd(self, data):
        self.send(data)
        return self.recv()

    # -- register access -------------------------------------------------
    def read_reg(self, n):
        r = self.cmd("p%x" % n)
        if r[:1] == b"E":
            return None
        return int.from_bytes(bytes.fromhex(r.decode()), "little")

    def write_reg(self, n, value):
        r = self.cmd("P%x=%s" % (n, value.to_bytes(4, "little").hex().encode()))
        if r != b"OK":
            raise RuntimeError("P%x rejected: %r" % (n, r))

    def read_pc(self):
        return self.read_reg(REG_PC)


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    target, spec = sys.argv[1], sys.argv[2]
    bp_s, _, entry_s = spec.partition(":")
    bp = int(bp_s, 0)
    entry = int(entry_s, 0)

    remote = Remote(target)

    # Ask for the stop reason; QEMU answers S/T for a stopped target.
    for _ in range(20):
        p = remote.cmd("?")
        if p[:1] in (b"S", b"T", b"W"):
            break
        time.sleep(0.1)

    r = remote.cmd("Z1,%x,4" % bp)
    if r != b"OK":
        sys.stderr.write("rsp_bypass: hardware breakpoint at 0x%x refused: %r\n"
                         % (bp, r))
        return 1
    dbg("armed, entering the continue loop")
    sys.stdout.write("rsp_bypass: hardware breakpoint armed at 0x%x -> 0x%x\n"
                     % (bp, entry))
    sys.stdout.flush()

    while True:
        # Continue; the only packets we care about are stop replies.
        remote.send("c")
        while True:
            p = remote.recv()
            if p[:1] in (b"T", b"S", b"W", b"X"):
                break
        if p[:1] in (b"W", b"X"):
            sys.stdout.write("rsp_bypass: guest exited (%r)\n" % p[:1])
            return 0

        pc = remote.read_pc()
        cpsr = remote.read_reg(REG_CPSR)
        if pc is None or cpsr is None:
            continue
        remote.write_reg(REG_R0, 0)
        remote.write_reg(REG_PC, entry)
        remote.write_reg(REG_CPSR, cpsr & ~CPSR_T)
        sys.stdout.write("bypass: pc=0x%08x r0=0x%08x cpsr=0x%08x\n"
                         % (pc, 0, cpsr & ~CPSR_T))
        sys.stdout.flush()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (EOFError, OSError) as e:
        sys.stderr.write("rsp_bypass: %s\n" % e)
        sys.exit(1)
