#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read and change the RouterBOOT "soft config" block in a MikroTik flash image.

RouterBOOT keeps its settings in a 4096-byte block that starts with the magic
`Soft`, followed by a CRC-32 (the field itself zeroed when it is computed), and
then a list of tagged values:

    u16 id | u16 length | value[length]

`0x06` is the RouterOS version that last wrote the block; the rest are the
settings `/system routerboard settings` shows.  The tag numbers are the ones
the OpenWrt `rbcfg`/`routerboot` soft_config driver used, and the value
encodings are its constants:

    0x01 uart_speed      0 = 115200, 1 = 57600, ... 8 = off
    0x02 boot_delay      seconds to wait for a key, 1..9
    0x03 boot_device     0 = eth, 1 = flasheth (NAND then ethernet),
                         3 = ethonce (ethernet once, then NAND),
                         5 = flash (NAND only), 7 = cfg, 8 = cfgonce
    0x04 boot_key        0 = any key, 1 = Delete
    0x05 cpu_mode        0 = powersave, 1 = regular
    0x06 bios_version    string
    0x09 boot_protocol   0 = bootp, 1 = dhcp
    0x0d booter          0 = regular, 1 = backup
    0x0f silent_boot     0/1

Anything this script does not know is carried through untouched, so a block
written by a newer RouterOS stays intact apart from what you asked to change.

    ./rbsoftcfg.py show  hapac3-nor.bin
    ./rbsoftcfg.py set --boot-device=ethonce --boot-delay=9 \
                        hapac3-nor.bin  hapac3-ethonce.bin

The image is never modified in place: `set` always writes a new file, and the
input may as well be the 4096-byte block on its own (`dd if=dump of=soft.bin
bs=4096 skip=$((0xbd000/4096)) count=1`), in which case the output is a block
too.  That is deliberate -- these bytes are the boot configuration of a real
board, so the only safe default is "write somewhere else".

The block is located by scanning for `Soft` at 4096-byte boundaries and keeping
the first one whose CRC checks out; a flash image can contain the string in
other places.
"""
import argparse
import os
import struct
import sys
import zlib

BLOCK = 4096
MAGIC = b"Soft"
CRC_OFF = 4

UART = {0: "115200", 1: "57600", 2: "38400", 3: "19200", 4: "9600",
        5: "4800", 6: "2400", 7: "1200", 8: "off"}
BOOT_DEVICE = {0: "eth", 1: "flasheth", 2: "cfcard", 3: "ethonce",
               5: "flash", 7: "cfg", 8: "cfgonce"}
BOOT_DEVICE_ID = {v: k for k, v in BOOT_DEVICE.items()}
BOOT_KEY = {0: "any", 1: "del"}
BOOT_PROTO = {0: "bootp", 1: "dhcp"}
BOOTER = {0: "regular", 1: "backup"}

# id -> (name, kind, decode table or None)
U32_TAGS = {
    0x01: ("uart_speed", UART),
    0x02: ("boot_delay", None),
    0x03: ("boot_device", BOOT_DEVICE),
    0x04: ("boot_key", BOOT_KEY),
    0x05: ("cpu_mode", {0: "powersave", 1: "regular"}),
    0x09: ("boot_protocol", BOOT_PROTO),
    0x0d: ("booter", BOOTER),
    0x0f: ("silent_boot", {0: "off", 1: "on"}),
}
NAMED_ONLY = {
    0x0a: "cpu_frequency",       # MHz
    0x0c: "cpu_freq_index",
    0x15: "hold_button",         # reformat-hold-button, seconds
    0x17: "hold_button_max",
    0x1b: "reformat_hold_button",
    0x1f: "protected_routerboot",  # 0x00140258 = 20 s min / 600 s max
}
VERSION_TAG = 0x06


def fail(msg):
    sys.stderr.write("rbsoftcfg: %s\n" % msg)
    raise SystemExit(2)


def crc_of(block):
    b = bytearray(block)
    b[CRC_OFF:CRC_OFF + 4] = b"\0\0\0\0"
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


def find_block(data):
    """First 4096-aligned `Soft` block whose CRC matches, or None."""
    off = 0
    while True:
        off = data.find(MAGIC, off)
        if off < 0:
            return None
        if off % BLOCK == 0 and off + BLOCK <= len(data):
            block = data[off:off + BLOCK]
            stored = struct.unpack_from("<I", block, CRC_OFF)[0]
            if stored == crc_of(block):
                return off, block
        off += 1


def parse(block):
    """[(id, value_bytes)] in file order, terminator excluded."""
    out, off = [], 8
    while off + 4 <= BLOCK:
        tid, tlen = struct.unpack_from("<HH", block, off)
        if tid == 0:
            break
        if off + 4 + tlen > BLOCK:
            fail("tag 0x%02x at 0x%x runs past the block" % (tid, off))
        out.append((tid, block[off + 4:off + 4 + tlen]))
        off += 4 + tlen
    return out


def show_tag(tid, val):
    name = NAMED_ONLY.get(tid)
    if tid in U32_TAGS:
        tag_name, table = U32_TAGS[tid]
        if len(val) != 4:
            return "%-20s %s" % (tag_name, val.hex())
        v = struct.unpack("<I", val)[0]
        text = table.get(v, str(v)) if table else str(v)
        return "%-20s %-10s (0x%x)" % (tag_name, text, v)
    if tid == VERSION_TAG:
        return "%-20s %s" % ("bios_version", val.rstrip(b"\0").decode("latin-1"))
    if name:
        if len(val) == 4:
            return "%-20s 0x%08x" % (name, struct.unpack("<I", val)[0])
        return "%-20s %s" % (name, val.hex())
    return "tag 0x%02x              %s" % (tid, val.hex())   # carried through


def cmd_show(args):
    data = open(args.file, "rb").read()
    hit = find_block(data)
    if hit is None:
        fail("%s holds no soft config block (no 4096-aligned `Soft` with a "
             "matching CRC)" % args.file)
    off, block = hit
    print("%s: soft config at 0x%x, crc %08x" %
          (args.file, off, struct.unpack_from("<I", block, CRC_OFF)[0]))
    for tid, val in parse(block):
        print("  %s" % show_tag(tid, val))
    return 0


def set_tag(tags, tid, payload):
    for i, (t, _) in enumerate(tags):
        if t == tid:
            tags[i] = (tid, payload)
            return
    tags.append((tid, payload))


def encode(tags):
    body = bytearray()
    for tid, val in tags:
        body += struct.pack("<HH", tid, len(val)) + val
    body += struct.pack("<HH", 0, 0)
    if 8 + len(body) > BLOCK:
        fail("the tags no longer fit in %d bytes" % BLOCK)
    return body


def cmd_set(args):
    data = open(args.input, "rb").read()
    hit = find_block(data)
    if hit is None:
        fail("%s holds no soft config block" % args.input)
    off, block = hit
    tags = parse(block)
    changed = []

    def want_u32(tid, value, name):
        old = next((struct.unpack("<I", v)[0] for t, v in tags if t == tid), None)
        if old == value:
            return
        set_tag(tags, tid, struct.pack("<I", value))
        changed.append("%s: %s -> %s" % (name, old if old is not None else "-",
                                         value))

    if args.boot_device is not None:
        want_u32(0x03, BOOT_DEVICE_ID[args.boot_device], "boot_device")
    if args.boot_delay is not None:
        if not 1 <= args.boot_delay <= 9:
            fail("boot_delay is 1..9 seconds, not %d" % args.boot_delay)
        want_u32(0x02, args.boot_delay, "boot_delay")
    if args.boot_key is not None:
        want_u32(0x04, {v: k for k, v in BOOT_KEY.items()}[args.boot_key],
                 "boot_key")
    if args.boot_protocol is not None:
        want_u32(0x09, {v: k for k, v in BOOT_PROTO.items()}[args.boot_protocol],
                 "boot_protocol")
    if args.uart_speed is not None:
        want_u32(0x01, {v: k for k, v in UART.items()}[args.uart_speed],
                 "uart_speed")
    if not changed:
        fail("nothing to change: no option differed")

    out = bytearray(block)
    body = encode(tags)
    out[8:] = body + bytes(BLOCK - 8 - len(body))
    struct.pack_into("<I", out, CRC_OFF, crc_of(out))

    image = bytearray(data)
    image[off:off + BLOCK] = out
    with open(args.output, "wb") as f:
        f.write(image)
    sys.stderr.write("rbsoftcfg: %s -> %s (block at 0x%x)\n"
                     % (args.input, args.output, off))
    for c in changed:
        sys.stderr.write("  %s\n" % c)
    sys.stderr.write("  crc %08x\n" % struct.unpack_from("<I", out, CRC_OFF)[0])
    return 0


def main(argv):
    p = argparse.ArgumentParser(
        prog="rbsoftcfg.py",
        description="Show or change the RouterBOOT soft config in a MikroTik "
                    "flash image (or in a 4096-byte block).")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("show", help="print the settings")
    s.add_argument("file")
    s.set_defaults(func=cmd_show)

    c = sub.add_parser("set", help="write a copy with settings changed")
    c.add_argument("input", help="flash image or a 4096-byte block")
    c.add_argument("output", help="file to write (never the input)")
    c.add_argument("--boot-device", choices=sorted(BOOT_DEVICE_ID),
                   help="eth, flasheth (NAND then ethernet), ethonce "
                        "(ethernet once, then NAND), flash (NAND only), "
                        "cfg, cfgonce")
    c.add_argument("--boot-delay", type=int,
                   help="seconds to wait for a key before booting (1..9)")
    c.add_argument("--boot-key", choices=["any", "del"])
    c.add_argument("--boot-protocol", choices=["bootp", "dhcp"])
    c.add_argument("--uart-speed", choices=["115200", "57600", "38400",
                                            "19200", "9600", "4800", "2400",
                                            "1200", "off"])
    c.set_defaults(func=cmd_set)

    args = p.parse_args(argv)
    if getattr(args, "output", None) and \
            os.path.abspath(args.output) == os.path.abspath(args.input):
        fail("refusing to overwrite the input: give it another name")
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
