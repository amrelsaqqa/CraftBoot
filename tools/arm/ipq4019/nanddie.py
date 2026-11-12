#!/usr/bin/env python3
"""Convert a NAND image between the two shapes that matter: partition and die.

A NAND die does not store pages back to back.  Every page is `page` bytes of
data followed by `oob` bytes of *spare* (out-of-band) area -- bad-block
markers, ECC parity, and on some vendors the per-block headers their boot ROM
reads.  So the byte layout of the chip is

    page 0 data | page 0 spare | page 1 data | page 1 spare | ...

while everything that talks to `/dev/mtdN` (cat, dd, `nanddump` without -o)
hands you only the data part:

    page 0 data | page 1 data | ...

That difference is the whole job of this script:

    ./nanddie.py to-die  mtd9.bin  nand-die.bin      # data-only -> whole chip
    ./nanddie.py to-dump nand-die.bin mtd9-again.bin # whole chip -> data-only
    ./nanddie.py info    whatever.bin                # which shape is this?

`to-die` fills the spare with an erased page (`--fill`, default 0xff) unless
`--spare` supplies the real spare bytes, and `to-dump --spare out.bin` splits
the spare off while it strips.  A dump taken with the OOB included can
therefore be taken apart and put back together without losing a byte.

Things worth knowing before you convert anything:

* `cat /dev/mtdN` and `dd if=/dev/mtdN` give data only -- the spare is missing
  and has to be synthesised.  That is fine for UBI: UBI keeps its EC and VID
  headers *in the data pages*, and this machine does no ECC.  It is *not* fine
  for a RouterBOOT/RouterOS kernel, whose per-block headers live in the spare;
  that needs a real chip dump.
* `nanddump -o` already writes this exact layout (data then OOB, page by page),
  so such a file needs no conversion at all.  Use `nanddump -o -n` (do not skip
  bad blocks): a dump that drops bad blocks is shorter than the chip and every
  block after the first bad one sits at the wrong offset.
* Erased spare is 0xff, never 0x00: a NAND's erased state is all ones, and
  zeros there read as a *programmed* spare -- garbage bad-block markers and
  block headers.
* A die is a whole number of erase blocks.  A data dump that is not becomes one
  only after `--force`, and the machine refuses the result.
* If the partition does not start at chip offset 0, `--at-page N` puts N erased
  pages in front of it, so the result is the whole chip and not just a slice.
* The spare is *interleaved*, never prepended: adding 4 MiB of padding at the
  front of a 128 MiB dump produces a 132 MiB file in which every page is
  misaligned.

Geometry defaults are the parts in this tree: 2048-byte pages with 64 bytes of
spare, 64 pages per erase block (a 132 KiB block, 128 MiB of data per die).
Override with --page / --oob / --ppblk for other parts:

    ./nanddie.py to-die --page 4096 --oob 128 --ppblk 128 dump.bin die.bin

Both directions stream, so a 128 MiB die does not have to fit in RAM, and
either file may be `-` for a pipe.
"""
import argparse
import hashlib
import os
import sys

PAGE = 2048
OOB = 64
PPBLK = 64
FILL = 0xFF
CHUNK = 1 << 20             # read buffer; pages are still handled one at a time


def fail(msg):
    sys.stderr.write("nanddie: %s\n" % msg)
    raise SystemExit(2)


def human(n):
    v = float(n)
    for unit in ("bytes", "KiB", "MiB", "GiB"):
        if v < 1024 or unit == "GiB":
            return ("%d bytes" % n) if unit == "bytes" else "%.1f %s" % (v, unit)
        v /= 1024.0


def blocks_str(pages, ppblk):
    if pages % ppblk:
        return "%d pages (not a whole block of %d)" % (pages, ppblk)
    blocks = pages // ppblk
    return "%d pages = %d %s of %d pages" % (
        pages, blocks, "block" if blocks == 1 else "blocks", ppblk)


class Reader:
    """A file (or stdin) that hands out fixed-size records."""

    def __init__(self, path, what):
        self.path = path
        self.what = what
        self.stdin = path == "-"
        self.f = sys.stdin.buffer if self.stdin else open(path, "rb",
                                                          buffering=CHUNK)
        self.n = 0

    def read(self, size):
        buf = self.f.read(size)
        if len(buf) != size:
            fail("%s: %s ended %d bytes into a %d-byte record -- it is not a "
                 "whole number of records" %
                 (self.path, self.what, len(buf), size))
        self.n += size
        return buf

    def at_end(self):
        return self.f.read(1) == b""

    def close(self):
        if not self.stdin:
            self.f.close()


class Writer:
    def __init__(self, path, what):
        self.path = path
        self.what = what
        self.stdout = path == "-"
        self.f = sys.stdout.buffer if self.stdout else open(path, "wb")
        self.n = 0

    def write(self, buf):
        self.f.write(buf)
        self.n += len(buf)

    def close(self):
        if self.stdout:
            self.f.flush()
        else:
            self.f.close()


def size_of(path):
    if path == "-":
        return None
    try:
        return os.stat(path).st_size
    except OSError as e:
        fail("%s: %s" % (path, e.strerror))


def parse_fill(text):
    try:
        v = int(text, 0)
    except ValueError:
        fail("--fill takes a byte value, not %r" % text)
    if not 0 <= v <= 255:
        fail("--fill must be 0..255, not %d" % v)
    return bytes([v])


def check_dump_size(path, size, page, oob):
    """A data-only image is whole pages; anything else is a different shape."""
    if size is None or size % page == 0:
        return
    hint = ""
    if size % (page + oob) == 0:
        hint = ("\n  This is a whole number of %d-byte records, which is a "
                "%d+%d page: it looks like a die already -- run `to-dump`, "
                "not `to-die`." % (page + oob, page, oob))
    fail("%s: %d bytes is not a whole number of %d-byte pages%s"
         % (path, size, page, hint))


def check_die_size(path, size, page, oob):
    if size is None:
        return
    stride = page + oob
    if size % stride == 0:
        return
    hint = ""
    if size % page == 0:
        hint = ("\n  It is a whole number of %d-byte pages but not of %d-byte "
                "pages: that looks like a data-only dump -- run `to-die`, not "
                "`to-dump`." % (page, stride))
    fail("%s: %d bytes is not a whole number of %d-byte pages (page %d + "
         "spare %d)%s" % (path, size, stride, page, oob, hint))


def hash_data_pages(path, page, oob, skip_pages=0):
    """sha256 over the data pages of a stream, skipping `oob` spare per page."""
    h = hashlib.sha256()
    f = open(path, "rb", buffering=CHUNK)
    for _ in range(skip_pages):
        f.read(page + oob)
    while True:
        data = f.read(page)
        if not data:
            break
        if len(data) != page:
            fail("%s ended inside a page (%d of %d bytes)" %
                 (path, len(data), page))
        h.update(data)
        if oob:
            f.read(oob)
    f.close()
    return h.hexdigest()


def cmd_to_die(args):
    page, oob, ppblk = args.page, args.oob, args.ppblk
    fill = parse_fill(args.fill)
    spare_page = fill * oob
    size = size_of(args.input)
    check_dump_size(args.input, size, page, oob)

    known_pages = None if size is None else size // page
    if known_pages is not None:
        total = known_pages + args.at_page
        if total % ppblk and not args.force:
            hint = ""
            if size % (page + oob) == 0:
                hint = ("\n  It is also a whole number of %d-byte records, "
                        "which is a %d+%d page: it looks like a die already -- "
                        "run `to-dump`, not `to-die`." % (page + oob, page, oob))
            fail("%s is %s, and --at-page=%d does not make it a whole erase "
                 "block either.  A die is always a whole number of blocks; use "
                 "--force to write a partial image anyway (the machine refuses "
                 "it).%s" % (args.input, blocks_str(known_pages, ppblk),
                             args.at_page, hint))

    spare = None
    if args.spare:
        spare_size = size_of(args.spare)
        if spare_size is not None:
            if spare_size % oob:
                fail("%s: %d bytes is not a whole number of %d-byte spare "
                     "records" % (args.spare, spare_size, oob))
            if known_pages is not None and spare_size // oob != known_pages:
                fail("%s holds spare for %d pages but %s has %d pages"
                     % (args.spare, spare_size // oob, args.input,
                        known_pages))
        spare = Reader(args.spare, "spare for %s" % args.input)

    src = Reader(args.input, args.input)
    dst = Writer(args.output, args.output)
    if args.at_page:
        erased = b"\xff" * page + spare_page
        for _ in range(args.at_page):
            dst.write(erased)

    h_in = hashlib.sha256()
    npages = 0
    while True:
        data = src.f.read(page)
        if not data:
            break
        if len(data) != page:
            dst.close()
            fail("%s ends inside a page (%d of %d bytes)"
                 % (args.input, len(data), page))
        src.n += len(data)
        npages += 1
        h_in.update(data)
        dst.write(data)
        dst.write(spare.read(oob) if spare is not None else spare_page)
    if spare is not None and not spare.at_end():
        dst.close()
        fail("%s has spare for more pages than %s has pages"
             % (args.spare, args.input))
    src.close()
    if spare is not None:
        spare.close()
    dst.close()

    total = npages + args.at_page
    if total % ppblk and not args.force:
        if args.output != "-":
            os.unlink(args.output)
        fail("%s is %s, which is not a whole erase block of %d pages.  The "
             "image has been removed; --force keeps it, and the machine will "
             "refuse it." % (args.input, blocks_str(npages, ppblk), ppblk))

    sys.stderr.write(
        "nanddie: %s -> %s\n"
        "  in:  %d data pages, %s\n"
        "  out: %s = %s of %d+%d bytes, spare %s%s\n" % (
            args.input, args.output,
            npages, human(npages * page),
            human(dst.n), blocks_str(total, ppblk), page, oob,
            "from %s" % args.spare if spare is not None else "0x%02x" % fill[0],
            ", %d erased pages in front" % args.at_page if args.at_page else ""))
    if spare is None:
        sys.stderr.write(
            "  the spare is synthesised: no block is marked bad, and no "
            "vendor block header exists in it\n")

    if args.verify and args.output != "-":
        if hash_data_pages(args.output, page, oob, args.at_page) != h_in.hexdigest():
            fail("%s does not read back as %s: the page data changed"
                 % (args.output, args.input))
        sys.stderr.write("  verified: every data page reads back unchanged\n")
    return 0


def cmd_to_dump(args):
    page, oob = args.page, args.oob
    size = size_of(args.input)
    check_die_size(args.input, size, page, oob)

    src = Reader(args.input, args.input)
    dst = Writer(args.output, args.output)
    spare = Writer(args.spare, "spare from %s" % args.input) if args.spare else None

    h_in = hashlib.sha256()
    npages = 0
    while True:
        data = src.f.read(page)
        if not data:
            break
        if len(data) != page:
            dst.close()
            fail("%s ends inside a page (%d of %d bytes)"
                 % (args.input, len(data), page))
        src.n += len(data)
        npages += 1
        h_in.update(data)
        dst.write(data)
        oob_bytes = src.read(oob)
        if spare is not None:
            spare.write(oob_bytes)
    src.close()
    dst.close()
    if spare is not None:
        spare.close()

    sys.stderr.write(
        "nanddie: %s -> %s\n"
        "  in:  %d pages of %d+%d bytes\n"
        "  out: %s%s\n" % (
            args.input, args.output, npages, page, oob,
            human(dst.n),
            ", spare also written to %s (%s)"
            % (args.spare, human(npages * oob)) if spare is not None else ""))
    if spare is None:
        sys.stderr.write(
            "  the spare is dropped: bad-block markers and any vendor block "
            "headers are gone, so a bootloader that needs them will not find "
            "them here\n")

    if args.verify and args.output != "-":
        if hash_data_pages(args.output, page, 0) != h_in.hexdigest():
            fail("%s does not read back as the data pages of %s"
                 % (args.output, args.input))
        sys.stderr.write("  verified: every data page read back unchanged\n")
    return 0


def cmd_info(args):
    page, oob, ppblk = args.page, args.oob, args.ppblk
    stride = page + oob
    size = size_of(args.file)
    if size is None:
        fail("info needs a file, not -")

    sys.stderr.write("nanddie: %s: %d bytes (%s)\n"
                     % (args.file, size, human(size)))

    die_pages = size // stride if size % stride == 0 else None
    dump_pages = size // page if size % page == 0 else None

    # A die is a whole number of erase blocks; a data-only dump of the same
    # length usually is not.  When both readings divide the size, the one that
    # is block-aligned is the one to believe, and a tie is reported as a tie.
    die_whole = die_pages is not None and die_pages % ppblk == 0
    dump_whole = dump_pages is not None and dump_pages % ppblk == 0

    if die_pages is not None and (die_whole or not dump_whole):
        sys.stderr.write("  die image: %s, %s of data in %d+%d-byte pages\n"
                         % (blocks_str(die_pages, ppblk),
                            human(die_pages * page), page, oob))
        sys.stderr.write("  `to-dump` strips it back to data pages\n")
        if dump_pages is not None:
            sys.stderr.write("  (the length also divides into %d %d-byte "
                             "pages; the die reading is the one that is a "
                             "whole number of erase blocks)\n"
                             % (dump_pages, page))
    elif dump_pages is not None:
        sys.stderr.write("  data-only dump: %s, %s of data, no spare\n"
                         % (blocks_str(dump_pages, ppblk), human(size)))
        sys.stderr.write("  `to-die` turns it into a chip image; the spare "
                         "will be synthesised unless --spare is given\n")
        if die_pages is not None:
            sys.stderr.write("  (the length also divides into %d %d-byte chip "
                             "pages of %d+%d; the dump reading is the one that "
                             "is a whole number of erase blocks)\n"
                             % (die_pages, stride, page, oob))
    else:
        sys.stderr.write("  neither: not a whole number of %d-byte data pages "
                         "nor of %d-byte chip pages\n" % (page, stride))
        return 1
    return 0


def main(argv):
    p = argparse.ArgumentParser(
        prog="nanddie.py",
        description="Convert between a NAND data dump and a whole-chip die "
                    "image (data pages with the spare area interleaved).",
        epilog="The module docstring covers the layout, the nanddump flags "
               "that already produce it, and what a synthesised spare does "
               "and does not give you.")
    geo = argparse.ArgumentParser(add_help=False)
    geo.add_argument("--page", type=int, default=PAGE,
                     help="data bytes per page (default %(default)s)")
    geo.add_argument("--oob", type=int, default=OOB,
                     help="spare bytes per page (default %(default)s)")
    geo.add_argument("--ppblk", type=int, default=PPBLK,
                     help="pages per erase block (default %(default)s)")
    sub = p.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("to-die", parents=[geo],
                       help="data-only dump -> chip image (spare interleaved)")
    d.add_argument("input", help="data-only dump, or - for stdin")
    d.add_argument("output", help="die image to write, or - for stdout")
    d.add_argument("--fill", default="0xff",
                   help="erased spare byte (default 0xff)")
    d.add_argument("--spare", metavar="SPARE",
                   help="real spare bytes to interleave (%(metavar)s holds "
                        "one oob-size record per page) instead of --fill")
    d.add_argument("--at-page", type=int, default=0, metavar="N",
                   help="put N erased pages in front (partition not at chip "
                        "offset 0)")
    d.add_argument("--force", action="store_true",
                   help="also write a die that is not a whole number of blocks")
    d.add_argument("--verify", action="store_true",
                   help="read the result back and compare every data page")
    d.set_defaults(func=cmd_to_die)

    u = sub.add_parser("to-dump", parents=[geo],
                       help="chip image -> data-only dump (spare stripped)")
    u.add_argument("input", help="die image, or - for stdin")
    u.add_argument("output", help="data-only dump to write, or - for stdout")
    u.add_argument("--spare", metavar="SPARE",
                   help="also write the stripped spare to %(metavar)s")
    u.add_argument("--verify", action="store_true",
                   help="read the result back and compare every data page")
    u.set_defaults(func=cmd_to_dump)

    i = sub.add_parser("info", parents=[geo],
                       help="say which shape a file has, and its geometry")
    i.add_argument("file")
    i.set_defaults(func=cmd_info)

    args = p.parse_args(argv)
    if args.page < 1 or args.oob < 0 or args.ppblk < 1:
        fail("--page and --ppblk must be at least 1, --oob at least 0")
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
