#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Minimal, strictly-synchronised GDB RSP client for guest introspection.

Written because ad-hoc socket code kept losing the "+" acknowledgements and
silently missing breakpoints.  Every packet here waits for its ack and reads
exactly one reply, so stop replies never get mixed up with command replies.
"""
import socket
import struct


class RSP:
    def __init__(self, host="127.0.0.1", port=1234, timeout=10, unix=None):
        if unix:
            self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.s.settimeout(timeout)
            self.s.connect(unix)
        else:
            self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self.buf = b""
        self.regs_per_word = 8

    # ---- low level ----
    def _read_exact_pkt(self):
        """Read one $...#xx packet, honour the ack, return (payload, interrupt)."""
        while True:
            while b"$" not in self.buf:
                chunk = self.s.recv(4096)
                if not chunk:
                    raise EOFError("gdbstub closed")
                self.buf += chunk
            i = self.buf.index(b"$")
            self.buf = self.buf[i:]
            if len(self.buf) < 2:
                continue
            j = self.buf.find(b"#")
            if j < 0 or len(self.buf) < j + 3:
                chunk = self.s.recv(4096)
                if not chunk:
                    raise EOFError("gdbstub closed")
                self.buf += chunk
                continue
            payload = self.buf[1:j]
            self.buf = self.buf[j + 3:]
            self.s.sendall(b"+")
            return payload.decode("latin1")

    def _send(self, body):
        data = body.encode()
        pkt = b"$" + data + b"#" + ("%02x" % (sum(data) & 0xFF)).encode()
        self.s.sendall(pkt)
        # wait for the ack (or an error)
        while True:
            if self.buf.startswith(b"+"):
                self.buf = self.buf[1:]
                return
            if self.buf.startswith(b"-"):
                raise RuntimeError("gdbstub NAK for %r" % body)
            if self.buf.startswith(b"$"):
                return  # a stub-initiated packet is pending; keep buffer
            chunk = self.s.recv(4096)
            if not chunk:
                raise EOFError("gdbstub closed")
            self.buf += chunk

    # ---- commands ----
    def cmd(self, body):
        self._send(body)
        return self._read_exact_pkt()

    def handshake(self):
        q = self.cmd("qSupported:multiprocess+;swbreak+;hwbreak+")
        return q

    def interrupt(self):
        self.s.sendall(b"\x03")
        return self._read_exact_pkt()

    def cont(self):
        # continue: no immediate reply, the stop reply comes later
        data = b"c"
        self.s.sendall(b"$" + data + b"#" + ("%02x" % (sum(data) & 0xFF)).encode())

    def wait_stop(self, timeout=120):
        self.s.settimeout(timeout)
        pkt = self._read_exact_pkt()
        self.s.settimeout(10)
        return pkt

    def bp(self, addr):
        return self.cmd("Z0,%x,4" % addr)

    def unbp(self, addr):
        return self.cmd("z0,%x,4" % addr)

    def regs(self):
        g = self.cmd("g")
        if not g:
            raise RuntimeError("empty register packet")
        n = len(g) // 8
        out = []
        for i in range(n):
            word = int(g[i * 8:(i + 1) * 8], 16)
            out.append(int.from_bytes(word.to_bytes(4, "big"), "little"))
        return out

    def pc(self):
        return self.regs()[37]

    def status(self):
        return self.regs()[32]

    def step(self):
        self._send("s")
        return self._read_exact_pkt()

    def mem(self, addr, n):
        return bytes.fromhex(self.cmd("m%x,%x" % (addr, n)))

    def close(self):
        self.s.close()


def main():
    r = RSP()
    print("stop:", r.interrupt())
    print("pc = %08x" % r.pc())
    r.close()


if __name__ == "__main__":
    main()
