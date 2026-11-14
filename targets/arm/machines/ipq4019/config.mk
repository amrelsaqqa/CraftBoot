# SPDX-License-Identifier: GPL-2.0-or-later
# Machine: -M ipq4019 (Qualcomm IPQ4019/IPQ4018, Cortex-A7).  Boards: default, hap-ac2, hap-ac3.
#
# The machine is split by controller: every source below is copied to the
# directory of the main destination with the same basename.  Adding a block
# means adding its file here, to the hook patch's meson list, and to
# ipq4019_internal.h if it is visible outside its own file.
ipq4019_MACHINE_DST  := hw/arm/ipq4019.c
ipq4019_MACHINE_SRCS := ipq4019.c ipq4019_gpio.c ipq4019_internal.h
ipq4019_BOARDS       := hap-ac2 hap-ac3
ipq4019_DEV_BRANCH   := ipq4019-model
