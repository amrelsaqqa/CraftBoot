#!/usr/bin/env python3
"""Decode the Linux 5.6 printk ring buffer out of a guest RAM dump.

    tools/ni-klog.py run/01/ram.bin               # whole buffer, in order
    tools/ni-klog.py run/01/ram.bin yaffs mtd     # only lines matching
    tools/ni-klog.py run/01/ram.bin --scan        # naive scan (fallback)

Why this exists: neither UART carries kernel output on the netinstall boot
(RouterOS keeps printk in the ring buffer only), so serial.log shows just the
installer UI.  The ring buffer is in RAM and survives a pmemsave.

Record layout in 5.6 (`struct printk_log`):

    u16 len        total record size
    u16 text_len   message length
    u16 dict_len   0 unless CONFIG_PRINTK_DICT
    u8  facility
    u8  flags:5, level:3
    u64 ts_nsec
    char text[text_len]

so records are self-describing: `len == text_len + dict_len + 16`, and the text
is printable.  We find the longest chain of such records and print it in order;
--scan falls back to grepping printable runs (noisy: it also picks up format
strings from .text).
"""
import re
import struct
import sys

HDR = 16


def plausible(data, pos):
    """Is there a sane chain of records starting at pos?"""
    ok = 0
    for _ in range(6):
        if pos + HDR > len(data):
            break
        ln, tl, dl, fac, fl = struct.unpack_from("<HHHBB", data, pos)
        if ln == 0 or ln > 4096 or tl == 0 or tl > ln or dl + HDR > ln:
            break
        if ln != tl + dl + HDR:
            break
        txt = data[pos + HDR:pos + HDR + tl]
        if not re.fullmatch(rb"[\x09\x0a\x20-\x7e\x1b]*", txt):
            break
        ok += 1
        pos += (ln + 7) & ~7          # records are 8-byte aligned
    return ok


def find_buffer(data):
    best, best_ok = None, 0
    pos = data.find(b"\x01\x00")
    while pos >= 0:
        ok = plausible(data, pos)
        if ok > best_ok:
            best, best_ok = pos, ok
        if best_ok >= 40:
            break
        pos = data.find(b"\x01\x00", pos + 4)
    return best


def decode(data, start):
    out = []
    pos = start
    while pos + HDR <= len(data):
        ln, tl, dl, fac, fl = struct.unpack_from("<HHHBB", data, pos)
        if not (ln and tl and tl <= ln and ln <= 4096 and ln == tl + dl + HDR):
            break
        txt = data[pos + HDR:pos + HDR + tl].decode("latin1", "replace")
        out.append((pos, txt))
        pos += (ln + 7) & ~7
    return out


def scan(data):
    run = re.compile(rb"[\x09\x0a\x20-\x7e]{4,}")
    for m in run.finditer(data):
        blob = m.group().decode("latin1")
        if re.search(r"[a-z]{3,}", blob) and not re.search(r"%[sdpxluz]|%0\d", blob):
            for line in blob.splitlines():
                if re.search(r"[a-z]{3,}", line):
                    yield m.start(), line


def main():
    argv = sys.argv[1:]
    if not argv:
        sys.exit(__doc__.splitlines()[0])
    naive = "--scan" in argv
    argv = [a for a in argv if a != "--scan"]
    path, wanted = argv[0], [w.lower() for w in argv[1:]]
    data = open(path, "rb").read()

    if naive:
        recs = list(scan(data))
    else:
        start = find_buffer(data)
        recs = decode(data, start) if start is not None else []
        if len(recs) < 8:
            recs = list(scan(data))

    for pos, txt in recs:
        for line in txt.splitlines():
            line = line.rstrip()
            if not line:
                continue
            if wanted and not any(w in line.lower() for w in wanted):
                continue
            print("%08x  %s" % (pos, line[:400]))


if __name__ == "__main__":
    main()
