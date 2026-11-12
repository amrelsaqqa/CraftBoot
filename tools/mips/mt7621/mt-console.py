#!/usr/bin/env python3
"""Drive the RouterOS console over the serial chardev, prompt by prompt.

Boots nothing itself: it attaches to the telnet chardev of a running VM, follows
the serial logfile, and answers each prompt exactly once:

    Login:                     -> admin
    Password:                  -> (empty: first boot has no password)
    press "Enter" to continue  -> Enter      (first-boot notice)
    new password>              -> 1234
    repeat new password>       -> 1234
    notice prompt / read it -> n

Once the [admin@MikroTik] prompt shows up it runs the network test commands and
prints the console transcript.
"""
import re
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4466
LOG = sys.argv[2] if len(sys.argv) > 2 else 'run/mt2/serial.log'
IFACE = sys.argv[3] if len(sys.argv) > 3 else 'ether1'
HOST = sys.argv[4] if len(sys.argv) > 4 else '192.168.1.2'


class Con:
    def __init__(self, port):
        self.port = port

    def send(self, data, settle=0.5):
        try:
            s = socket.create_connection(('127.0.0.1', self.port), timeout=5)
        except OSError as e:
            print('serial connect failed:', e)
            return False
        s.sendall(data)
        time.sleep(settle)
        s.close()
        return True


PROMPTS = [
    (b'Login:', b'admin\r'),
    (b'Password:', b'\r'),
    (b'press "Enter" to continue', b'\r'),
    (b'new password>', b'1234\r'),
    (b'repeat new password>', b'1234\r'),
    # the guest's own prompt text; nothing here interprets licences
    (b'software license', b'n\r'),
]


def main():
    con = Con(PORT)
    pos = 0
    text = b''
    last_send = {}          # prompt -> time of last reply
    deadline = time.time() + 240
    ready = False

    while time.time() < deadline:
        try:
            with open(LOG, 'rb') as f:
                f.seek(pos)
                chunk = f.read()
                pos = f.tell()
        except OSError:
            chunk = b''
        if chunk:
            text += chunk
        if b'[admin@MikroTik] >' in text[-400:]:
            ready = True
            break

        tail = text[-400:]
        # act on the *last* prompt visible, so redraws cannot double-fire
        best = None
        for needle, reply in PROMPTS:
            idx = tail.rfind(needle)
            if idx < 0:
                continue
            if best is None or idx > best[0]:
                best = (idx, needle, reply)
        if best is not None:
            _, needle, reply = best
            now = time.time()
            if now - last_send.get(needle, 0) > 4.0:
                last_send[needle] = now
                time.sleep(0.4)
                con.send(reply, settle=0.6)

        time.sleep(0.4)

    if not ready:
        print('did not reach the RouterOS prompt')
        print(text.decode('latin1', 'replace')[-1200:])
        return 1

    for cmd in ('/interface print',
                '/ip address print',
                '/ip address add address=192.168.1.1/24 interface=%s' % IFACE,
                '/ip address print',
                '/ping %s count=3' % HOST):
        con.send(cmd.encode() + b'\r', settle=1.0)
        time.sleep(5 if 'ping' in cmd else 2.5)

    time.sleep(1)
    try:
        text += open(LOG, 'rb').read()[pos:]
    except OSError:
        pass
    print('=== transcript tail ===')
    for line in text.decode('latin1', 'replace').splitlines()[-60:]:
        if line.strip():
            print('   ', line.rstrip()[:160])
    return 0


if __name__ == '__main__':
    sys.exit(main())
