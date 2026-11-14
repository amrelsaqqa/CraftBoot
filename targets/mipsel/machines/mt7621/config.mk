# SPDX-License-Identifier: GPL-2.0-or-later
# Machine: -M mt7621 (MediaTek MT7621A, MIPS 1004Kc).
# Board presets are runtime options: -M mt7621,board=<preset>
mt7621_MACHINE_SRCS := mt7621.c:hw/mips/mt7621.c
mt7621_BOARDS      := lg-gapm7200 rb750gr3
mt7621_DEV_BRANCH  := mt7621-model
