# SPDX-License-Identifier: GPL-2.0-or-later
# Machine: -M ipq4019 (Qualcomm IPQ4019/IPQ4018, Cortex-A7).  Boards: default, hap-ac2, hap-ac3.
#
# Every source is a <source>:<destination> pair, relative to the machine
# directory and the QEMU tree.  The machine file stays in hw/arm; the blocks
# live under hw/arm/ipq4019/ so their includes are plain ("ipq4019.h") and the
# build tree groups them the way the repository does.
ipq4019_MACHINE_SRCS := \
    ipq4019.c:hw/arm/ipq4019.c \
    ipq4019.h:hw/arm/ipq4019/ipq4019.h \
    modules/gpio.c:hw/arm/ipq4019/gpio.c \
    modules/uart.c:hw/arm/ipq4019/uart.c \
    modules/bam.c:hw/arm/ipq4019/bam.c \
    modules/eth.c:hw/arm/ipq4019/eth.c \
    modules/stub.c:hw/arm/ipq4019/stub.c
ipq4019_BOARDS       := hap-ac2 hap-ac3
ipq4019_DEV_BRANCH   := ipq4019-model
