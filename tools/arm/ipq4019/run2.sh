#!/bin/bash
# Same machine, different dump: the U-Boot + OpenWrt image for the same SoC.
#
#   ./run2.sh                print the whole SBL1 boot log for this dump
#   ./run2.sh 30             same, but give up after 30 s
#   DUMP=/other/dump ./run2.sh   override which dump this boots
#
# Everything else (output, RAML, QEMU_IPQ_* knobs) is run.sh's.
exec env DUMP="${DUMP:-$HOME/dump-708ipq-24.10.0-8mb-minimal-extroot-caraboot.bin}" \
    "$(dirname "$0")/run.sh" "$@"
