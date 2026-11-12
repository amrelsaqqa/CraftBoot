#!/usr/bin/env python3
"""
panel-vnc.py -- watch the board's lamps through QEMU's VNC server.

The LED panel is a QEMU display console (see the machine's `ipq4019-led-panel`),
which means any display backend shows it.  This build is headless, so the matrix
reads the panel's pixels with QMP `screendump`; this tool is the other proof --
it talks raw RFB to a `-display vnc=...` machine and saves what a VNC viewer
would have on screen, with no viewer installed.

    ./build-ui/qemu-system-arm -M 'ipq4019,leds=status:5:al;lan:7' \\
        -display vnc=127.0.0.1:7 -monitor none -serial null &
    ./tools/panel-vnc.py 127.0.0.1:7 --out /tmp/panel.png

It is deliberately a tiny RFB client -- version 3.8, security type None, one
raw framebuffer update -- because that is all this needs to be, and it keeps
the test free of a VNC library.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import socket
import struct
import sys

# The RFB protocol's message types, only the ones a server may send first.
SET_COLOUR_MAP = 1
BELL = 2
SERVER_CUT_TEXT = 3
FRAMEBUFFER_UPDATE = 0


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("the server closed the connection")
        buf += chunk
    return buf


def handshake(sock):
    """Version and security, then pick "None" -- QEMU's default without a
    password.  Returns (width, height, name)."""
    version = recv_exact(sock, 12)
    if not version.startswith(b"RFB "):
        raise ValueError("not a VNC server: %r" % version)
    major, minor = (int(x) for x in version[4:11].split(b"."))
    if (major, minor) < (3, 8):          # QEMU speaks 3.8; 3.3 is the floor
        raise ValueError("unsupported RFB version %d.%d" % (major, minor))
    sock.sendall(b"RFB 003.008\n")

    ntypes = recv_exact(sock, 1)[0]
    if ntypes == 0:
        raise ValueError("the server refused: %s" %
                         recv_exact(sock, 4).decode("latin-1"))
    types = recv_exact(sock, ntypes)
    if 1 not in types:
        raise ValueError("the server wants authentication (%r); start it "
                         "without a VNC password" % types)
    sock.sendall(b"\x01")                # security type 1: None

    # Version 3.8 sends SecurityResult even for None.
    result = struct.unpack(">I", recv_exact(sock, 4))[0]
    if result != 0:
        raise ValueError("security failed: %d" % result)

    # QEMU reads the "shared desktop" flag *before* it sends ServerInit (it
    # registers a one-byte reader for it, the way the original VNC did), so a
    # client that waits for ServerInit first hangs.  Shared = 1: join rather
    # than ask for exclusive access.
    sock.sendall(b"\x01")

    width, height, _pixfmt, name_len = struct.unpack(
        ">HH16sI", recv_exact(sock, 24))
    name = recv_exact(sock, name_len).decode("latin-1")
    return width, height, name


def set_pixel_format(sock):
    """32 bits per pixel, 8 bits per channel, true colour, RGB order.  This is
    what a QEMU console surface uses, so the pixels arrive unchanged."""
    sock.sendall(b"\0\0\0\0" + struct.pack(">BBBBHHHBBB3x",
                                            32, 24, 0, 1, 255, 255, 255,
                                            16, 8, 0))


def read_update(sock, want_pixels):
    """Read messages until a framebuffer update arrives; return
    (x, y, w, h, payload) for the first raw rectangle."""
    while True:
        mtype = recv_exact(sock, 1)[0]
        if mtype == FRAMEBUFFER_UPDATE:
            recv_exact(sock, 1)                       # padding
            nrects = struct.unpack(">H", recv_exact(sock, 2))[0]
            rects = []
            for _ in range(nrects):
                x, y, w, h, enc = struct.unpack(">HHHHi", recv_exact(sock, 12))
                if enc != 0:
                    raise ValueError("the server sent encoding %d, not raw" % enc)
                data = recv_exact(sock, w * h * want_pixels)
                rects.append((x, y, w, h, data))
            if rects:
                return rects
        elif mtype == SET_COLOUR_MAP:
            recv_exact(sock, 1)
            first, n = struct.unpack(">HH", recv_exact(sock, 4))
            recv_exact(sock, n * 6)
        elif mtype == BELL:
            pass
        elif mtype == SERVER_CUT_TEXT:
            recv_exact(sock, 3)
            length = struct.unpack(">I", recv_exact(sock, 4))[0]
            recv_exact(sock, length)
        else:
            raise ValueError("unexpected RFB message %d" % mtype)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1].strip())
    ap.add_argument("host", help="host:port of the VNC server")
    ap.add_argument("--out", default="panel.png", help="PNG to write")
    ap.add_argument("--timeout", type=float, default=10.0)
    opts = ap.parse_args(argv)

    host, _, port = opts.host.partition(":")
    sock = socket.create_connection((host, int(port or 5900)),
                                    timeout=opts.timeout)
    try:
        width, height, name = handshake(sock)
        set_pixel_format(sock)
        sock.sendall(bytes([2, 0]) + struct.pack(">Hi", 1, 0))     # raw only
        sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))
        rects = read_update(sock, 4)
    finally:
        sock.close()

    # Compose the rectangles into one image; a full-screen update is one rect,
    # but a redrawn row is a small one, so both are honoured.
    pixels = [(0, 0, 0)] * (width * height)
    for x, y, w, h, data in rects:
        for row in range(h):
            off = row * w * 4
            for col in range(w):
                r, g, b = data[off + col * 4 + 2], data[off + col * 4 + 1], \
                    data[off + col * 4]
                if 0 <= y + row < height and 0 <= x + col < width:
                    pixels[(y + row) * width + x + col] = (r, g, b)

    try:
        from PIL import Image
    except ImportError:
        print("%dx%d from %r: %d rect(s), install Pillow to write a PNG"
              % (width, height, name, len(rects)))
        return 0

    img = Image.new("RGB", (width, height))
    img.putdata(pixels)
    img.save(opts.out)
    print("%s: %dx%d from %r, %d rect(s)" % (opts.out, width, height, name,
                                             len(rects)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
