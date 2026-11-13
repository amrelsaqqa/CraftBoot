#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Convert between flat NAND dumps and full-die dumps.

Both layouts describe the same chip; they only differ in whether the 64-byte
spare area of each 2 KiB page is stored:

    flat dump   2048 bytes per page   (128 MiB for a 128 MiB chip)
    full die    2048 + 64 bytes/page  (132 MiB for the same chip)

The machine accepts either (it picks the layout from the file size, see
`mt7621_nand_load`), and `MT7621_NAND_IMAGE=` can store a persistent copy so
writes survive a restart.  This tool moves between the two, and reports the
bad-block markers that only exist in the spare area.

Usage:
    nand-die.py info <image>
    nand-die.py die2dump <die> <dump> [--oob <oob.bin>]
    nand-die.py dump2die <dump> <die> [--oob-from <die>]

`dump2die` fills the spare area with 0xff (erased / not bad) unless
`--oob-from` copies the spare bytes from an existing die image, which is the
only way to keep real bad-block markers and ECC when rebuilding a die.
"""
import os
import sys

PAGE = 2048
OOB = 64
STRIDE = PAGE + OOB
PAGES_PER_BLOCK = 64            # 128 KiB erase blocks


def classify(size):
    """Return (kind, pages, note) for an image of this size."""
    if size % STRIDE == 0:
        pages = size // STRIDE
        if pages and not (pages & (pages - 1)):
            return 'die', pages, 'spare area present (2048+64 bytes/page)'
    if size % PAGE == 0:
        pages = size // PAGE
        if pages and not (pages & (pages - 1)):
            return 'dump', pages, 'main area only (2048 bytes/page)'
    return 'unknown', 0, 'size is neither 2048 nor 2112 bytes/page'


def info(path):
    size = os.path.getsize(path)
    kind, pages, note = classify(size)
    print('%s: %d bytes (%.1f MiB)' % (path, size, size / (1 << 20)))
    print('  layout: %s - %s' % (kind, note))
    if not pages:
        return 1
    blocks = pages // PAGES_PER_BLOCK
    print('  pages: %d, erase blocks: %d (of %d pages)' %
          (pages, blocks, PAGES_PER_BLOCK))
    if kind != 'die':
        print('  bad blocks: unknown without the spare area')
        return 0
    with open(path, 'rb') as f:
        bad = []
        for blk in range(blocks):
            f.seek(blk * PAGES_PER_BLOCK * STRIDE + PAGE)
            oob = f.read(2)
            if len(oob) == 2 and oob != b'\xff\xff':
                bad.append((blk, oob))
    print('  bad blocks: %d%s' % (len(bad),
          ''.join(' %d(0x%02x%02x)' % (b, o[0], o[1]) for b, o in bad[:8])))
    return 0


def die2dump(die, dump, oob_path=None):
    size = os.path.getsize(die)
    kind, pages, _ = classify(size)
    if kind != 'die':
        print('%s is not a full-die image (%s)' % (die, kind))
        return 1
    with open(die, 'rb') as src, open(dump, 'wb') as dst:
        oob_f = open(oob_path, 'wb') if oob_path else None
        try:
            for _ in range(pages):
                page = src.read(STRIDE)
                dst.write(page[:PAGE])
                if oob_f:
                    oob_f.write(page[PAGE:])
        finally:
            if oob_f:
                oob_f.close()
    print('wrote %s (%d bytes, %d pages)' % (dump, pages * PAGE, pages))
    if oob_path:
        print('wrote %s (%d bytes of spare data)' % (oob_path, pages * OOB))
    return 0


def dump2die(dump, die, oob_from=None):
    size = os.path.getsize(dump)
    kind, pages, _ = classify(size)
    if kind != 'dump':
        print('%s is not a flat dump (%s)' % (dump, kind))
        return 1
    if oob_from and os.path.getsize(oob_from) != pages * STRIDE:
        print('%s does not hold %d pages of die data' % (oob_from, pages))
        return 1
    src_oob = open(oob_from, 'rb') if oob_from else None
    erase = b'\xff' * OOB
    try:
        with open(dump, 'rb') as src, open(die, 'wb') as dst:
            for n in range(pages):
                page = src.read(PAGE)
                if src_oob:
                    src_oob.seek(n * STRIDE + PAGE)
                    oob = src_oob.read(OOB)
                else:
                    oob = erase
                dst.write(page + oob)
    finally:
        if src_oob:
            src_oob.close()
    print('wrote %s (%d bytes, %d pages, spare %s)' %
          (die, pages * STRIDE, pages,
           'copied from ' + oob_from if oob_from else 'filled with 0xff'))
    return 0


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == 'info':
        return info(argv[2])
    if cmd in ('die2dump', 'dump2die'):
        if len(argv) < 4:
            print(__doc__)
            return 2
        rest = argv[4:]
        opt = None
        if rest:
            if rest[0] not in ('--oob', '--oob-from') or len(rest) < 2:
                print(__doc__)
                return 2
            opt = rest[1]
        if cmd == 'die2dump':
            return die2dump(argv[2], argv[3], opt)
        return dump2die(argv[2], argv[3], opt)
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
