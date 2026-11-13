#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Read a guest kernel's printk ring buffer out of the emulated RAM.

Why this exists
---------------
The RouterOS 7 mmips kernel in the MikroTik RB750Gr3 dump does not put any
of its messages on UART0: `console=ttyS0,115200` is on the kernel command
line but the console never registers (and on a fault the board restarts
before anything could be printed anyway).  All the interesting output --
version banner, memory map, Oops/panic backtraces -- only ever lands in
printk's in-memory log buffer.

So instead of watching the serial port we snapshot guest RAM right after
the kernel has run for a moment and dig the ring buffer out of the dump.

Flow
----
  1. QEMU is started with `-monitor unix:<sock>,server,nowait`.
  2. We poll `info registers` until the PC is in kernel space
     (0xc0000000..0xc1000000 for this kernel) or, with --wait, just sleep.
  3. `pmemsave <phys> <size> "<file>"` writes a RAM window to a host file.
     pmemsave takes *physical* addresses: the kernel's 0x80000000/0xc0000000
     virtual addresses alias to physical 0x00000000.
  4. The dump is scanned for the log buffer, which is a plain ring of text
     with "<N>" loglevel prefixes.  We anchor on "<5>Linux version" and walk
     out to the enclosing printable run, printing the result.

The buffer is in .bss low in the first 32 MiB for this kernel; the default
window (0x100000..0x2000000) is enough and keeps the dump small.

Usage
-----
  tools/klog.py                          # default RB750Gr3 run
  tools/klog.py --wait 20                # plain sleep instead of PC poll
  tools/klog.py --qemu-arg MT7621_CFG1=0xd2d2 --qemu-arg MT7621_TRACE=
  tools/klog.py --dump /tmp/ram.bin      # dump only, parse later:
  tools/klog.py --parse /tmp/ram.bin     # parse an existing dump
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_QEMU = os.path.join(ROOT, "build", "qemu-system-mipsel")
DEFAULT_IMAGE = os.path.join(ROOT, "RB750Gr3_6F3807CD31FE_704_r3.bin")

ANCHORS = [b"<5>Linux version", b"<6>Linux version", b"Linux version"]


class Monitor:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(path)
        self.sock.settimeout(30)
        # Drain the banner prompt so the next command's response can be
        # recognised by its own trailing prompt.
        self.cmd("")

    def cmd(self, text):
        self.sock.sendall((text + "\n").encode())
        data = b""
        while True:
            chunk = self.sock.recv(65536)
            if not chunk:
                break
            data += chunk
            if data.rstrip().endswith(b"(qemu)"):
                break
        return data.decode("utf-8", "replace")

    def pc(self):
        out = self.cmd("info registers")
        m = re.search(r"pc=0x([0-9a-fA-F]+)", out)
        return int(m.group(1), 16) if m else None

    def close(self):
        self.sock.close()


def wait_for_socket(path, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(path):
            try:
                return Monitor(path)
            except OSError:
                pass
        time.sleep(0.05)
    raise SystemExit("klog: QEMU monitor socket never appeared")


def snapshot(mon, args, path):
    """pmemsave one RAM window and wait for the file to land."""
    if os.path.exists(path):
        os.unlink(path)
    resp = mon.cmd('pmemsave 0x%x 0x%x "%s"'
                   % (args.phys, args.size, path))
    end = time.time() + 60
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) >= args.size:
            return True
        if "invalid" in resp.lower():
            print("klog: pmemsave rejected: %s" % resp, file=sys.stderr)
            return False
        time.sleep(0.05)
    return False


def launch(args, sock):
    env = dict(os.environ)
    for item in args.qemu_arg:
        if "=" in item:
            k, v = item.split("=", 1)
            if v == "":
                env.pop(k, None)
            else:
                env[k] = v
        else:
            env[item] = "1"

    cmd = [args.qemu,
           "-M", args.machine,
           "-m", args.ram,
           "-kernel", args.kernel,
           "-display", "none",
           "-monitor", "unix:%s,server,nowait" % sock,
           "-serial", "file:%s" % args.console] + args.extra

    print("klog: %s" % " ".join(cmd), file=sys.stderr)
    log = open(args.qemu_log, "ab")
    return subprocess.Popen(cmd, env=env, stdout=log, stderr=log)


def wait_kernel(mon, args):
    """Block until the guest PC is in kernel space (or --wait elapses)."""
    deadline = time.time() + args.timeout
    if args.wait is not None:
        time.sleep(args.wait)
    else:
        while time.time() < deadline:
            pc = mon.pc()
            if pc is not None and args.pc_lo <= pc < args.pc_hi:
                break
            time.sleep(0.05)
        else:
            print("klog: kernel PC never appeared, dumping anyway",
                  file=sys.stderr)
        time.sleep(args.settle)


def run_and_dump(args):
    sock = os.path.join(tempfile.gettempdir(), "klog-%d.sock" % os.getpid())
    for p in (sock, args.dump):
        if os.path.exists(p):
            os.unlink(p)
    proc = launch(args, sock)

    try:
        mon = wait_for_socket(sock)
        wait_kernel(mon, args)
        snapshot(mon, args, args.dump)
        mon.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        if os.path.exists(sock):
            os.unlink(sock)

    end = time.time() + 10
    while time.time() < end and not os.path.exists(args.dump):
        time.sleep(0.05)


def run_watch(args):
    """
    Keep the guest running and print whatever printk gained every N seconds.

    This is the "chase the ring buffer" mode: no reboot of the guest between
    snapshots, no lost panic, and every new line is shown exactly once.
    """
    sock = os.path.join(tempfile.gettempdir(), "klog-%d.sock" % os.getpid())
    path = args.dump
    for p in (sock, path):
        if os.path.exists(p):
            os.unlink(p)
    proc = launch(args, sock)
    out = open(args.out, "a", buffering=1) if args.out else None
    seen = []
    try:
        mon = wait_for_socket(sock)
        wait_kernel(mon, args)
        end = time.time() + args.duration
        while time.time() < end and proc.poll() is None:
            if not snapshot(mon, args, path):
                break
            text = log_text(path)
            lines = [l for l in text.splitlines() if l.strip()]
            new = []
            if seen:
                for i in range(len(lines) - 1, -1, -1):
                    if lines[i] == seen[-1]:
                        new = lines[i + 1:]
                        break
                else:
                    new = lines[-40:]
            else:
                new = lines
            for line in new:
                if out:
                    out.write(line + "\n")
                else:
                    print(line)
            if out:
                out.flush()
            else:
                sys.stdout.flush()
            seen = lines
            time.sleep(args.interval)
        mon.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        if os.path.exists(sock):
            os.unlink(sock)
        if out:
            out.close()
    return 0


def printable(ch):
    return 32 <= ch < 127 or ch in (9, 10, 13)


def find_log(data):
    """
    Locate printk's ring buffer.

    The anchor ("<5>Linux version") is the fast path, but the ring is
    circular: once it wraps the oldest lines (including the banner) are gone,
    so fall back to the run of text with the highest density of "<N>"
    loglevel prefixes.
    """
    for a in ANCHORS:
        i = data.find(a)
        if i >= 0:
            return i

    best = None
    best_score = 0
    start = None
    holes = 0
    for i, ch in enumerate(data):
        if printable(ch):
            if start is None:
                start = i
            holes = 0
        elif start is not None:
            holes += 1
            if holes > 32:
                run = data[start:i - holes + 1]
                score = len(re.findall(rb"<[0-7]>", run))
                if score > best_score:
                    best, best_score = start, score
                start = None
    if best is not None and best_score >= 4:
        return best
    return None


def log_text(path):
    """Return the printable printk run found in a RAM dump."""
    data = open(path, "rb").read()
    anchor = find_log(data)
    if anchor is None:
        return ""
    start = anchor
    holes = 0
    while start > 0 and holes < 4:
        start -= 1
        holes = 0 if printable(data[start]) else holes + 1
    start += holes
    end = anchor
    holes = 0
    while end < len(data) and holes < 32:
        end += 1
        holes = 0 if printable(data[end - 1]) else holes + 1
    return data[start:end].decode("utf-8", "replace").replace("\r", "")


def parse(path, only_tail=False):
    data = open(path, "rb").read()

    anchor = find_log(data)
    if anchor is None:
        print("klog: no printk buffer found in %s" % path, file=sys.stderr)
        print("klog: dumped window was phys 0x0+file offset; try a bigger "
              "--size", file=sys.stderr)
        return 1

    # Walk out of the enclosing printable run: the ring is text separated by
    # NUL padding, so stop at a short non-printable stretch.
    start = anchor
    holes = 0
    while start > 0 and holes < 4:
        start -= 1
        if printable(data[start]):
            holes = 0
        else:
            holes += 1
    start += holes

    end = anchor
    holes = 0
    while end < len(data) and holes < 32:
        end += 1
        if printable(data[end - 1]):
            holes = 0
        else:
            holes += 1

    text = data[start:end].decode("utf-8", "replace")
    text = text.replace("\r", "")
    if only_tail:
        text = text[-only_tail:]
    print(text)

    if b"Oops" in data[start:end] or b"panic" in data[start:end]:
        # Cheap hint: the interesting part is usually the last 100 lines.
        if not only_tail:
            print("klog: Oops/panic present; re-run with --tail 4000 to see "
                  "just the end", file=sys.stderr)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", default=DEFAULT_QEMU)
    ap.add_argument("--kernel", default=DEFAULT_IMAGE)
    ap.add_argument("--machine", default="mt7621,board=rb750gr3")
    ap.add_argument("--ram", default="256M")
    ap.add_argument("--console", default="/tmp/klog-serial.log")
    ap.add_argument("--qemu-log", default="/tmp/klog-qemu.log")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[],
                    help="extra QEMU arguments (must be last)")
    ap.add_argument("--qemu-arg", action="append", default=[],
                    metavar="NAME=VALUE", help="environment for the guest run")
    ap.add_argument("--wait", type=float, default=None,
                    help="sleep this long before dumping instead of polling PC")
    ap.add_argument("--settle", type=float, default=0.6,
                    help="extra delay after the kernel is first seen")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--phys", type=lambda s: int(s, 0), default=0x00100000)
    ap.add_argument("--size", type=lambda s: int(s, 0), default=0x00400000,
                    help="RAM window to snapshot (keep it small: pmemsave "
                         "stalls the guest, and a stall of >20 s of guest "
                         "time trips the softlockup watchdog)")
    ap.add_argument("--pc-lo", type=lambda s: int(s, 0), default=0xc0000000)
    ap.add_argument("--pc-hi", type=lambda s: int(s, 0), default=0xc1000000)
    ap.add_argument("--dump", default="/tmp/klog-ram.bin")
    ap.add_argument("--parse", metavar="FILE",
                    help="do not start QEMU, parse an existing dump")
    ap.add_argument("--tail", type=int, default=0,
                    help="print only the last N characters of the log")
    ap.add_argument("--watch", action="store_true",
                    help="keep the guest running and print new printk lines")
    ap.add_argument("--out", metavar="FILE",
                    help="append the log lines to FILE (tail -f friendly); "
                         "defaults to stdout")
    ap.add_argument("--interval", type=float, default=30.0,
                    help="seconds between snapshots in --watch mode")
    ap.add_argument("--duration", type=float, default=1800.0,
                    help="how long to watch, in seconds")
    args = ap.parse_args()

    if args.parse:
        return parse(args.parse, args.tail)

    if args.watch:
        return run_watch(args)

    run_and_dump(args)
    return parse(args.dump, args.tail)


if __name__ == "__main__":
    sys.exit(main())
