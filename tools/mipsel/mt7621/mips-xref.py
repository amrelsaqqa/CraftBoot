#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Find rodata references inside a flat MIPS image (Breed payload).

Linear-sweeps the image with capstone, tracks lui/addiu materialisation of
constants per register, and reports every instruction that leaves a register
pointing into the strings window.  Also dumps the surrounding instructions so
the caller can be inspected.

    tools/mipsel/mt7621/mips-xref.py <image> <base-hex> <target-lo-hex> <target-hi-hex>
"""
import sys

import capstone
from capstone import mips as cm

img = open(sys.argv[1], 'rb').read()
base = int(sys.argv[2], 16)
lo = int(sys.argv[3], 16)
hi = int(sys.argv[4], 16)

md = capstone.Cs(capstone.CS_ARCH_MIPS,
                 capstone.CS_MODE_MIPS32 | capstone.CS_MODE_LITTLE_ENDIAN)
md.detail = True

code = bytes(img)
regval = {}
hits = []
insns = {}

for ins in md.disasm(code, base):
    pc = ins.address
    ops = ins.operands
    # forget the previous value whenever a register is written in a new way
    if ins.id in (cm.MIPS_INS_LUI,) and len(ops) == 2 and \
            ops[0].type == capstone.CS_OP_REG and \
            ops[1].type == capstone.CS_OP_IMM:
        regval[ins.reg_name(ops[0].reg)] = (ops[1].imm & 0xffff) << 16
    elif ins.id in (cm.MIPS_INS_ADDIU, cm.MIPS_INS_ADDI,
                    cm.MIPS_INS_ORI) and len(ops) == 3:
        dst = ins.reg_name(ops[0].reg)
        src = ins.reg_name(ops[1].reg)
        imm = ops[2].imm
        if src in regval:
            val = (regval[src] + (imm if ins.id != cm.MIPS_INS_ORI
                                  else (imm & 0xffff))) & 0xffffffff
            regval[dst] = val
            if lo <= val < hi:
                hits.append((pc, val, ins.mnemonic + ' ' + ins.op_str))
        else:
            regval.pop(dst, None)
    elif ops and ops[0].type == capstone.CS_OP_REG and ins.mnemonic.split('.')[0] in (
            'addu', 'subu', 'and', 'or', 'xor', 'nor', 'slt', 'sltu', 'sll',
            'srl', 'sra', 'sllv', 'srlv', 'srav', 'lw', 'lb', 'lbu', 'lh',
            'lhu', 'lwl', 'lwr', 'mfhi', 'mflo', 'movz', 'movn', 'mfc0',
            'seb', 'seh', 'ext', 'ins', 'rotr', 'not', 'neg', 'negu'):
        regval.pop(ins.reg_name(ops[0].reg), None)

print('hits:', len(hits))
for pc, val, text in hits[:20]:
    print('  %08x -> %08x   %s' % (pc, val, text))

# Also report references to the window with a direct "%hi/%lo" pattern that
# starts from a fresh lui (belt and braces).
print('\nnearby pc for first hits:')
for pc, val, text in hits[:6]:
    start = pc - 0x10
    for ins in md.disasm(img[start - base:start - base + 0x40], start):
        mark = '>>' if ins.address == pc else '  '
        print('  %s %08x  %-8s %s' % (mark, ins.address, ins.mnemonic, ins.op_str))
    print()
