#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Summarise a MT7621_SPILOG trace: which opcodes the guest uses, how many
bytes each transfer moves, and how the read addresses advance.

    MT7621_SPILOG=1 qemu ... 2>spi.log
    tools/spi-scan.py spi.log
"""
import re
import sys
from collections import Counter

EXEC = re.compile(r"spi: exec trans=([0-9a-f]{8}) more=([0-9a-f]{8}) "
                  r"op=([0-9a-f]{8}) cmd_len=(\d+) mosi_len=(\d+) rx_len=(\d+)")


def flash_addr(opcode, cmd_len):
    """Legacy command word: opcode in bits 7:0, address MSB first above."""
    op = opcode & 0xff
    if op in (0x03, 0x0b):
        return ((opcode >> 24) << 16) | ((opcode >> 8) & 0xffff)
    return None


def main(path):
    ops = Counter()
    lengths = Counter()
    reads = []
    for line in open(path, errors="replace"):
        m = EXEC.search(line)
        if not m:
            continue
        trans, more, opcode, cmd_len, mosi_len, rx_len = (
            int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16),
            int(m.group(4)), int(m.group(5)), int(m.group(6)))
        ops[opcode & 0xff] += 1
        lengths[(cmd_len, rx_len)] += 1
        a = flash_addr(opcode, cmd_len)
        if a is not None:
            reads.append(a)

    print("opcodes:", ", ".join("%02x x%d" % (k, v) for k, v in ops.most_common()))
    print("transfers (cmd bytes, rx bytes):",
          ", ".join("%s x%d" % (k, v) for k, v in lengths.most_common(6)))
    if reads:
        print("reads: %d, window %06x..%06x" % (len(reads), min(reads), max(reads)))
        steps = Counter(b - a for a, b in zip(reads, reads[1:]))
        print("steps:", ", ".join("%d x%d" % (k, v) for k, v in steps.most_common(6)))
        print("first 16:", " ".join("%06x" % a for a in reads[:16]))
        print("last 16: ", " ".join("%06x" % a for a in reads[-16:]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/spi.log"))
