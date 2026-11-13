#!/usr/bin/env python3
"""
Symbolise kernel addresses from a guest RAM dump.

The RouterOS mmips kernel is stripped (no ELF symbols survive the dump) but it
carries kallsyms, so every address on a task stack can be resolved exactly as
the kernel itself would print it (that is what the "ra : 80118da0
local_r4k_flush_icache_range+0x70" lines in an Oops come from).

kallsyms layout (no CONFIG_KALLSYMS_BASE_RELATIVE on this build):

    kallsyms_addresses[]   ascending u32 addresses
    kallsyms_num_syms      u32 == len(addresses)   (16-byte aligned after it)
    kallsyms_names         stream of [len][tokens...]
    kallsyms_markers[]     one u32 per 256 symbols
    kallsyms_token_table   256 NUL terminated strings
    kallsyms_token_index   256 u16 offsets into token_table

Finding it needs no symbols: look for the long strictly-ascending array of
kernel addresses, take the u32 right after it as the symbol count, then find
the token table by trying candidates until the decoded names look like C
identifiers.

Usage:
    tools/ksyms.py --dump ram.bin --build            # cache the table
    tools/ksyms.py --dump ram.bin 0x80371adc 0x80109148
    tools/ksyms.py --dump ram.bin --task 0x8fc20000  # symbolise a task stack
"""
import argparse
import bisect
import json
import os
import re
import struct
import sys

CACHE = "/tmp/ksyms.json"


def u32(d, p):
    return struct.unpack_from("<I", d, p)[0]


def u16(d, p):
    return struct.unpack_from("<H", d, p)[0]


def find_arrays(d):
    """Return (addresses_offset, num_syms, names_offset)."""
    n = len(d)
    i = 0x100000
    best = None
    while i < n - 4:
        v = u32(d, i)
        if 0x80000000 <= v < 0x90000000:
            j = i
            cnt = 0
            prev = 0
            while j < n - 4:
                w = u32(d, j)
                if not (0x80000000 <= w < 0x90000000) or (cnt and w < prev):
                    break
                prev = w
                cnt += 1
                j += 4
            if cnt > 2000 and (best is None or cnt > best[1]):
                # num_syms sits at the next 16-byte boundary
                ns = (j + 15) & ~15
                if u32(d, ns) == cnt:
                    best = (i, cnt, ns + 4)
            i = j if j > i else i + 4
        else:
            i += 4
    if not best:
        raise SystemExit("ksyms: no kallsyms address table found")
    return best


def walk_names(d, names0, nsym):
    offs = []
    p = names0
    for _ in range(nsym):
        offs.append(p - names0)
        p += 1 + d[p]
    return offs, (p + 15) & ~15


def parse_tokens(d, tok):
    q = tok
    toks = []
    for _ in range(256):
        e = d.find(b"\x00", q, q + 40)
        if e < 0:
            return None
        s = d[q:e]
        if any(c < 32 or c > 126 for c in s):
            return None
        toks.append(s.decode("latin1"))
        q = e + 1
    size = q - tok
    ti = (q + 1) & ~1
    idx = [u16(d, ti + 2 * k) for k in range(256)]
    if any(idx[k] > idx[k + 1] for k in range(255)) or idx[-1] >= size:
        return None
    return toks


def build(d):
    addr0, nsym, names0 = find_arrays(d)
    offs, markers = walk_names(d, names0, nsym)
    nk = (nsym + 255) // 256 + 1
    start = markers + 4 * nk
    for tok in range(start, start + 0x2000):
        toks = parse_tokens(d, tok)
        if not toks:
            continue
        sample = []
        for k in range(8):
            p = names0 + offs[k]
            ln = d[p]
            sample.append("".join(toks[b] for b in d[p + 1:p + 1 + ln]))
        if all(re.fullmatch(r"[A-Za-z0-9_.$]+", s) for s in sample):
            syms = []
            for k in range(nsym):
                p = names0 + offs[k]
                ln = d[p]
                nm = "".join(toks[b] for b in d[p + 1:p + 1 + ln])
                syms.append((u32(d, addr0 + 4 * k), nm[1:]))
            syms.sort()
            return syms
    raise SystemExit("ksyms: token table not found")


def load(dump, rebuild=False):
    if not rebuild and os.path.exists(CACHE):
        return json.load(open(CACHE))
    syms = build(open(dump, "rb").read())
    json.dump(syms, open(CACHE, "w"))
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default="/tmp/g.bin")
    ap.add_argument("--build", action="store_true", help="rebuild the cache")
    ap.add_argument("--task", type=lambda s: int(s, 0),
                    help="symbolise every kernel text pointer on a task stack")
    ap.add_argument("--text-lo", type=lambda s: int(s, 0), default=0x80000000)
    ap.add_argument("--text-hi", type=lambda s: int(s, 0), default=0x80600000)
    ap.add_argument("addrs", nargs="*", type=lambda s: int(s, 0))
    args = ap.parse_args()

    d = open(args.dump, "rb").read()
    syms = load(args.dump, args.build)
    addrs = [a for a, _ in syms]

    def sym(v):
        i = bisect.bisect_right(addrs, v) - 1
        if i < 0:
            return "?"
        a, n = syms[i]
        return "%s+0x%x" % (n, v - a)

    if args.task:
        st = struct.unpack_from("<I", d, (args.task & 0x1fffffff) + 4)[0]
        print("task %08x stack %08x" % (args.task, st))
        for i in range(0, 0x2000, 4):
            v = u32(d, st + i)
            if args.text_lo <= v < args.text_hi:
                print("  %08x  %s" % (st + i, sym(v)))
        return 0

    for a in args.addrs:
        print("%08x %s" % (a, sym(a)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
