#!/usr/bin/env python3
"""Send commands to a guest console exposed as a telnet chardev.

Usage:  guest.py <telnet-port> <logfile> [cmd ...]

Each command is written to the chardev and the logfile is tailed until the
shell prompt comes back (or a timeout expires).  Commands are separated by
sending one at a time so the transcript stays readable.
"""
import os
import socket
import sys
import time

PORT = int(sys.argv[1])
LOG = sys.argv[2]
CMDS = sys.argv[3:]
PROMPT = b'root@OpenWrt:/#'
SETTLE = 2.0


def send(data, wait):
    s = socket.create_connection(('127.0.0.1', PORT), timeout=5)
    s.sendall(data)
    time.sleep(wait)
    s.close()


def tail(pos):
    try:
        with open(LOG, 'rb') as f:
            f.seek(pos)
            chunk = f.read()
            return chunk, f.tell()
    except OSError:
        return b'', pos


def main():
    pos = os.path.getsize(LOG) if os.path.exists(LOG) else 0
    for cmd in CMDS:
        send(cmd.encode() + b'\r', 0.3)
        deadline = time.time() + 12
        out = b''
        while time.time() < deadline:
            chunk, pos = tail(pos)
            out += chunk
            # wait until the prompt reappears after the command echo
            if out.count(PROMPT) >= 2:
                break
            time.sleep(0.2)
        text = out.decode('latin1', 'replace')
        sys.stdout.write('$ %s\n%s\n' % (cmd, text.rstrip()))
    return 0


if __name__ == '__main__':
    sys.exit(main())
