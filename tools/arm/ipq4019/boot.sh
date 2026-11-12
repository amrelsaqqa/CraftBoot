#!/bin/bash
# boot.sh [dump] [seconds] [appsbl-entry]
#
# Boot a raw flash dump past SBL1 and print the whole UART log: SBL1's own
# boot log, then APPSBL (U-Boot or RouterBOOT) up to autoboot, including the
# FIT image it loads from flash.
#
# SBL1 hands over to QSEE, whose TZ bring-up needs the QCOM protection fabric
# (NoC/XPU/VMIDMT) that this machine does not model yet.  So, as in enter.sh,
# the PC is moved to APPSBL at the moment QSEE would start.  Unlike a patch,
# nothing in flash is modified: APPSBL runs from the bytes SBL1 loaded.
#
# The APPSBL entry is the only image-dependent input.  It is the address SBL1
# puts APPSBL at (printed by "APPSBL Image Loaded"); the table below covers the
# two shipped dumps and can always be overridden on the command line.
#
#   QEMU_IPQ_DTB=/path/board.dtb ./boot.sh     # ports as the board DTS says
#   QEMU_IPQ_ETH_PORTS="0:1000,1:!1000" ./boot.sh
#   QEMU_IPQ_FLASH_SIZE=0x800000 ./boot.sh      # 8 MiB die instead of 32 MiB
set -u
cd "$(dirname "$0")" || exit 1

DUMP=${1:-$HOME/dump-708ipq-24.10.0-8mb-minimal-extroot-caraboot.bin}
SECS=${2:-40}
ENTRY=${3:-}

if [ -z "$ENTRY" ]; then
    if grep -qa "RouterBOARD" "$DUMP"; then
        ENTRY=0x80000000        # RouterBOOT, loaded by the MikroTik SBL1
    elif grep -qa "Caraboot" "$DUMP"; then
        ENTRY=0x87300000        # U-Boot, loaded by the OpenWrt SBL1
    else
        echo "boot.sh: don't know where SBL1 puts APPSBL in this image." >&2
        echo "         pass the address SBL1 printed in 'APPSBL Image Loaded'," >&2
        echo "         e.g. ./boot.sh $DUMP 30 0x80000000" >&2
        exit 1
    fi
fi

echo "# $(basename "$DUMP") -> APPSBL entry $ENTRY"
./enter.sh "$DUMP" "$ENTRY" "$SECS"
