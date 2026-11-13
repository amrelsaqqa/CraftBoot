#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rebuild the appended DTB of the MikroTik netinstall kernel.

Deletes the LED node (to free space), enables uartlite@c00, sets stdout-path and
a longer bootargs, then pads the blob back to the original totalsize so nothing
else in the image moves.
"""
import struct
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else "netinboot-mmips"
DST = sys.argv[2] if len(sys.argv) > 2 else "netinboot-mmips.dtb"
BOOTARGS = ("console=ttyS0,115200 earlycon=uart8250,mmio32,0x1e000c00 "
            "loglevel=8 ignore_loglevel")

d = bytearray(open(SRC, "rb").read())
base = d.find(b"\xd0\x0d\xfe\xed")
assert base > 0, "no DTB found"
(total, off_struct, off_strings, off_rsv, ver, lastc, cpuid,
 size_str, size_struct) = struct.unpack_from(">9I", d, base + 4)
print("DTB at %#x totalsize %#x struct@%#x strings@%#x" %
      (base, total, off_struct, off_strings))

strings = d[base + off_strings:base + off_strings + size_str]


def s_at(o):
    e = strings.index(b"\x00", o)
    return strings[o:e].decode("latin1")


class Node:
    def __init__(self, name):
        self.name = name
        self.props = []          # (name, bytes)
        self.kids = []


root = Node("")
stack = [root]
p = base + off_struct
while True:
    tok = struct.unpack_from(">I", d, p)[0]
    p += 4
    if tok == 1:                                # FDT_BEGIN_NODE
        e = d.index(b"\x00", p)
        n = Node(d[p:e].decode("latin1"))
        p = (e + 4) & ~3
        stack[-1].kids.append(n)
        stack.append(n)
    elif tok == 2:                              # FDT_END_NODE
        stack.pop()
    elif tok == 3:                              # FDT_PROP
        ln, no = struct.unpack_from(">II", d, p)
        p += 8
        val = bytes(d[p:p + ln])
        p = (p + ln + 3) & ~3
        stack[-1].props.append((s_at(no), val))
    elif tok == 4:                              # FDT_NOP
        pass
    elif tok == 9:                              # FDT_END
        break


# the structure block starts with FDT_BEGIN_NODE for the (empty-named) root
if len(root.kids) == 1 and root.kids[0].name == "":
    root = root.kids[0]


def find(path):
    cur = root
    for part in [x for x in path.split("/") if x]:
        cur = next(k for k in cur.kids if k.name == part)
    return cur


def setprop(node, name, val):
    node.props = [(n, v) for (n, v) in node.props if n != name]
    node.props.append((name, val))


# drop the LED node entirely (frees space, not needed for a serial bring-up)
leds = [k for k in root.kids if k.name.startswith("rb750gr3-leds")]
root.kids = [k for k in root.kids if not k.name.startswith("rb750gr3-leds")]
print("removed nodes:", [k.name for k in leds])

uart = find("palmbus@1e000000/uartlite@c00")
setprop(uart, "status", b"okay\x00")
chosen = find("chosen")
setprop(chosen, "bootargs", BOOTARGS.encode() + b"\x00")
setprop(chosen, "stdout-path", b"/palmbus@1e000000/uartlite@c00\x00")

# ---- serialise ----
strtab = bytearray()
stroff = {}
out = bytearray()


def name_off(n):
    if n not in stroff:
        stroff[n] = len(strtab)
        strtab.extend(n.encode() + b"\x00")
    return stroff[n]


def emit(node):
    out.extend(struct.pack(">I", 1))
    out.extend(node.name.encode() + b"\x00")
    while len(out) % 4:
        out.push if False else out.append(0)
    for n, v in node.props:
        out.extend(struct.pack(">III", 3, len(v), name_off(n)))
        out.extend(v)
        while len(out) % 4:
            out.append(0)
    for k in node.kids:
        emit(k)
    out.extend(struct.pack(">I", 2))


emit(root)
out.extend(struct.pack(">I", 9))

hdr = 0x28
rsv = 16                       # one reserved-memory terminator (2x u64)
off_struct_new = hdr + rsv
off_strings_new = off_struct_new + len(out)
total_new = off_strings_new + len(strtab)
print("new totalsize %#x (old %#x)" % (total_new, total))
assert total_new <= total, "rebuilt DTB does not fit - delete more nodes"

blob = bytearray(total)        # zero-filled, i.e. ljust to the original size
struct.pack_into(">10I", blob, 0,
                 0xd00dfeed, total, off_struct_new, off_strings_new,
                 hdr, ver, lastc, cpuid, len(strtab), len(out))
blob[off_struct_new:off_struct_new + len(out)] = out
blob[off_strings_new:off_strings_new + len(strtab)] = strtab
d[base:base + total] = blob
open(DST, "wb").write(d)
print("written:", DST, "(DTB padded to %#x)" % total)
