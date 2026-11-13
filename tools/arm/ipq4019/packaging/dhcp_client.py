#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""dhcp_client.py -- a DHCP client small enough to ship with the launcher.

The guest is the DHCP server (it is a router), the host is the client, and the
only thing the host wants is an address on the guest's LAN.  ISC dhclient does
that job, but it is one-shot, needs a pid file, and inside a user namespace it
fails its own capability checks -- none of which matters for a 100 line client
that speaks the same four messages.

  python3 dhcp_client.py tap0 [deadline-seconds]

On success it prints one line of shell assignments and exits 0:

  ip=192.168.1.123 prefix=24 router=192.168.1.1 dns=192.168.1.1 lease=43200

so the caller can `eval` it and add the address itself.  Diagnostics go to
stderr; a timeout is exit 1.
"""

import os
import random
import socket
import struct
import sys
import time

MAGIC = b"\x63\x82\x53\x63"
DISCOVER, OFFER, REQUEST, ACK, NAK = 1, 2, 3, 5, 6


def mac_of(iface):
    with open("/sys/class/net/%s/address" % iface) as f:
        return bytes(int(b, 16) for b in f.read().strip().split(":"))


def build(xid, mac, msgtype, requested=None, server=None, hostname=None):
    p = struct.pack(
        "!BBBBIHH4s4s4s4s16s64s128s",
        1, 1, 6, 0,               # op BOOTREQUEST, htype ethernet, hlen 6
        xid, 0, 0x8000,           # broadcast answer: we have no address yet
        b"\0" * 4, b"\0" * 4, b"\0" * 4, b"\0" * 4,
        mac + b"\0" * 10, b"\0" * 64, b"\0" * 128)
    o = bytes([53, 1, msgtype])
    o += bytes([55, 4, 1, 3, 6, 51])            # mask, router, dns, lease
    o += bytes([57, 2]) + struct.pack("!H", 1500)
    if hostname:
        h = socket.gethostname().encode()[:32]
        o += bytes([12, len(h)]) + h
    if requested:
        o += bytes([50, 4]) + socket.inet_aton(requested)
    if server:
        o += bytes([54, 4]) + socket.inet_aton(server)
    return p + MAGIC + o + b"\xff"


def parse(data):
    if len(data) < 240 or data[236:240] != MAGIC:
        return None
    opts, i = {}, 240
    while i < len(data):
        t = data[i]
        if t == 255:
            break
        if t == 0:
            i += 1
            continue
        ln = data[i + 1]
        opts[t] = data[i + 2:i + 2 + ln]
        i += 2 + ln
    return {
        "xid": struct.unpack("!I", data[4:8])[0],
        "yiaddr": socket.inet_ntoa(data[16:20]),
        "opts": opts,
    }


def mask_to_prefix(mask):
    bits = 0
    for byte in mask:
        bits += bin(byte).count("1")
    return bits


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    iface = sys.argv[1]
    deadline = time.time() + float(sys.argv[2] if len(sys.argv) > 2 else 20)

    mac = mac_of(iface)
    xid = random.getrandbits(32)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except OSError:
            pass
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    # Only listen on the interface under test: a broadcast would otherwise be
    # picked up on every link the host has.
    if hasattr(socket, "SO_BINDTODEVICE"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE,
                     iface.encode() + b"\0")
    s.bind(("0.0.0.0", 68))
    s.settimeout(1.0)

    def xchg(pkt, want, what):
        """Send pkt until a reply of type `want` arrives (or the deadline)."""
        sent = 0
        while time.time() < deadline:
            if time.time() - sent > 2.0:
                s.sendto(pkt, ("255.255.255.255", 67))
                sent = time.time()
            try:
                data, _ = s.recvfrom(4096)
            except socket.timeout:
                continue
            r = parse(data)
            if not r or r["xid"] != xid:
                continue
            t = r["opts"].get(53, b"\0")[0]
            if t == NAK:
                sys.stderr.write("dhcp: NAK in %s\n" % what)
                return None
            if t != want:
                continue
            return r
        sys.stderr.write("dhcp: no %s within the deadline\n" % what)
        return None

    offer = xchg(build(xid, mac, DISCOVER), OFFER, "OFFER")
    if not offer:
        return 1
    srv = socket.inet_ntoa(offer["opts"].get(54, b"\0\0\0\0"))
    ack = xchg(build(xid, mac, REQUEST, offer["yiaddr"], srv), ACK, "ACK")
    if not ack:
        return 1

    o = ack["opts"]
    mask = o.get(1, b"\xff\xff\xff\x00")
    router = socket.inet_ntoa(o[3][:4]) if 3 in o else ""
    dns = " ".join(socket.inet_ntoa(o[6][i:i + 4])
                   for i in range(0, len(o.get(6, b"")), 4))
    lease = struct.unpack("!I", o[51][:4])[0] if 51 in o else 0
    print("ip=%s prefix=%d router=%s dns=%s lease=%d"
          % (ack["yiaddr"], mask_to_prefix(mask), router, dns, lease))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except OSError as e:
        sys.stderr.write("dhcp: %s\n" % e)
        sys.exit(1)
