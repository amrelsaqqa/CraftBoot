#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Boot a raw flash dump (SBL1 first) on the ipq4019 machine and print its log.
#
#   ./run.sh                     boot the default dump, print the whole UART log
#   ./run.sh /path/to/dump.bin   any other dump of this SoC
#   ./run.sh 60                  give up after 60 s (old-style: a bare number
#                                is still a timeout, not a dump path)
#   ./run.sh /path/dump.bin 60   both
#   RAML=1 ./run.sh              SBL1 only: also dump its in-RAM log copy
#   STARTING=150 ./run.sh        wait longer after "Starting services..." for
#                                RouterOS to panic and reset
#   SBL1_ONLY=1 ./run.sh         stop at "SBL1, End" (no QSEE redirect)
#   QEMU_IPQ_BYPASS=0x..:0x..    override the QSEE redirect for a new SBL1
#   QEMU_IPQ_TRACE=1 ./run.sh    log every stubbed MMIO access (-d unimp)
#   QEMU_IPQ_RESET_DUMP=/tmp/ram.bin ./run.sh
#                                dump DRAM as the machine resets: a panic the
#                                guest does not print stays readable there
#
# The whole log comes out of the UART, so the script only ever prints that:
# QEMU's own messages go to stderr and a separate file, and the guest's output
# is captured on stdout.  Nothing in the dump is decoded, repacked or patched.
# The machine models the hardware the dump talks to (SPI NOR behind the BLSP1
# QUP/BAM pair, the switch, PHYs, EDMA, UART), so any dump of this SoC runs as
# far as its own software does.
#
# QSEE/TZ is not modelled yet: its bring-up needs the QCOM protection fabric,
# and QSEE enables the MMU within a few instructions.  So the handover from SBL1
# into QSEE is redirected to APPSBL by the resident gdb agent in bypass.sh --
# the harness only, never the image.  The address pair is the one image-specific
# input, and it is picked from the dump's own text; override it when a new SBL1
# build moves it.
set -u
cd "$(dirname "$0")" || exit 1
. ./bypass.sh

DUMP=${DUMP:-$HOME/mikrotik-dump-latest7.17-32m.bin}
MAXSECS=${MAXSECS:-180}
IDLE=${IDLE:-20}                # stop after this many quiet seconds
# RouterOS prints "Starting services..." and then keeps working silently for
# several seconds (the login prompt is at the end of that), so this window has
# to be at least as long as the idle one or the log would stop before the
# prompt -- and before a panic that follows it.
STARTING=${STARTING:-$((IDLE * 5))}
SERIAL=${SERIAL:-/tmp/run.ser}
FIFO=/tmp/run.in
ERRLOG=/tmp/run.err
GDB=/tmp/run.gsock
BOOTLOG=/tmp/run.ram.bin
ENTRYBNB=

# A bare number is a timeout (the historical interface); anything else is a
# dump path.  Both may be given, in either order.
for arg in "$@"; do
    case $arg in
        ''|*[!0-9]*) DUMP=$arg ;;
        *) MAXSECS=$arg ;;
    esac
done

SBL1_ONLY=${SBL1_ONLY:-0}
[ "${RAML:-0}" = 1 ] && SBL1_ONLY=1

# The handover point and where the dump puts APPSBL are image properties, so
# bypass.sh classifies them on the machine ("auto") instead of us matching
# strings in the dump.  An explicit <bp>:<entry> still wins.
QEMU_IPQ_BYPASS=${QEMU_IPQ_BYPASS:-auto}

rm -f "$SERIAL" "$FIFO" "$ERRLOG" "$GDB" "$BOOTLOG"
: > "$SERIAL"
mkfifo "$FIFO"

# RAML forces SBL1-only (the copy stops early, and one gdbstub serves one
# client), but it still needs the stub for a single memory read after the boot.
BYPASS=1
[ "$SBL1_ONLY" = 1 ] && BYPASS=0
[ "${RAML:-0}" = 1 ] && BYPASS=0

if [ "$BYPASS" = 1 ]; then
    GDBARGS="-S -gdb unix:$GDB,server=on,wait=off"
elif [ "${RAML:-0}" = 1 ]; then
    GDBARGS="-gdb unix:$GDB,server=on,wait=off"   # no -S: the machine must run
else
    GDBARGS=""
fi

# BOOTDEV_TYPE=6 -> SMEM/QCA flash-type enum: 6 = SPI NOR (2 would mean NAND,
# which makes U-Boot take the NAND env path and BUG), so SBL1.s boot_flash_target
# actually probes the flash instead of skipping it.
QEMU_IPQ_BOOTDEV_TYPE=${QEMU_IPQ_BOOTDEV_TYPE:-6} \
./build/qemu-system-arm -M ipq4019 \
    -kernel "$DUMP" \
    -display none -serial stdio -monitor none \
    $GDBARGS -d int,guest_errors -D /tmp/run.int \
    < "$FIFO" > "$SERIAL" 2> "$ERRLOG" &
QP=$!
exec 3>"$FIFO"

cleanup() {
    bypass_stop
    kill -9 $QP 2>/dev/null
    wait $QP 2>/dev/null
    exec 3>&-
    rm -f "$FIFO"
}
trap cleanup EXIT INT TERM

if ! kill -0 $QP 2>/dev/null; then
    echo "run.sh: QEMU did not stay up:" >&2
    sed 's/^/  /' "$ERRLOG" >&2
    exit 1
fi

if [ "$BYPASS" = 1 ]; then
    bypass_start "$GDB" "$ERRLOG" /tmp/run.gdb.log "$QEMU_IPQ_BYPASS" || {
        echo "run.sh: the QSEE redirect never fired; see /tmp/run.gdb.log" >&2
        echo "run.sh: use SBL1_ONLY=1 for a pure SBL1 log, or set" >&2
        echo "        QEMU_IPQ_BYPASS=<handover>:<appsbl-entry>" >&2
        exit 1
    }
fi

# Watch the UART: the console only answers after the switch driver has
# configured its ports, so type a CR once that line appears (and again if the
# shell still has not answered).  Stop at a shell prompt, at a known stall, on
# a quiet line, or when the time budget runs out.
START_NS=$(date +%s%N)
KICKED=0
STALLED=""
LAST_SIZE=0
QUIET=0
while :; do
    SIZE=$(wc -c < "$SERIAL" 2>/dev/null || echo 0)
    if [ "$SIZE" != "$LAST_SIZE" ]; then
        LAST_SIZE=$SIZE
        QUIET=0
    else
        QUIET=$((QUIET + 1))
    fi

    if grep -qa "root@OpenWrt" "$SERIAL" 2>/dev/null; then
        STALLED="shell"
        break
    fi
    if [ "$SBL1_ONLY" = 1 ] &&
       grep -qaE "SBL1, End|Boot error ocuured" "$SERIAL" 2>/dev/null; then
        STALLED="sbl1-end"
        break
    fi
    if grep -qa "switch wan: configuring" "$SERIAL" 2>/dev/null; then
        if [ "$KICKED" -lt 3 ] || [ "$QUIET" -ge 5 ]; then
            printf '\r' >&3
            KICKED=$((KICKED + 1))
            QUIET=0
        fi
    fi
    # RouterOS prints this and then goes quiet.  It is not a stall though: it
    # panics and resets a moment later, and the reset makes the machine boot
    # again from SBL1 (the machine keeps the flash, so this is a real reboot).
    # So watch for a second boot cycle -- that is the panic -- and give the
    # image STARTING quiet ticks to reach it before calling it a stall.
    if grep -qa "Starting services..." "$SERIAL" 2>/dev/null; then
        [ -z "${STS_NS:-}" ] && STS_NS=$(date +%s%N)
        if [ "$(grep -ac 'SBL1, Start' "$SERIAL" 2>/dev/null)" -ge 2 ]; then
            STALLED="rebooted"
            REBOOT_AFTER=$(printf '%d.%01ds' $(( ($(date +%s%N) - STS_NS) / 1000000000 )) \
                                                $(( (($(date +%s%N) - STS_NS) / 100000000) % 10 )))
            break
        fi
        if [ "$QUIET" -ge "${STARTING:-10}" ]; then
            STALLED="starting-services"
            break
        fi
    fi
    # RouterOS ends its boot at the login prompt and then goes quiet for good.
    # A panic after that point still shows up as a second boot cycle, which the
    # check above catches, so the quiet window is safe to use here.
    if grep -qa "MikroTik Login:" "$SERIAL" 2>/dev/null &&
       [ "$QUIET" -ge $((IDLE * 5)) ]; then
        STALLED="login"
        break
    fi
    if [ "$QUIET" -ge $((IDLE * 5)) ] && [ "$SIZE" -gt 0 ]; then
        STALLED="quiet"
        break
    fi
    if [ $(( ($(date +%s%N) - START_NS) / 1000000000 )) -ge "$MAXSECS" ]; then
        STALLED="timeout"
        break
    fi
    kill -0 $QP 2>/dev/null || { STALLED="qemu-exited"; break; }
    sleep 0.2
done
MS=$(( ($(date +%s%N) - START_NS) / 1000000 ))
ELAPSED=$(printf '%d.%01ds' $((MS / 1000)) $(((MS % 1000) / 100)))

if [ "${RAML:-0}" = 1 ]; then
    # The in-RAM copy stops at pm_driver_init (the ~33rd entry), so it is only
    # a cross-check of the early stages.  It needs the gdbstub, which is why
    # RAML implies SBL1_ONLY: one stub, one client.
    gdb -batch -ex "target remote $GDB" \
        -ex "dump binary memory $BOOTLOG 0x0a0e0000 0x0a0f1000" \
        >/dev/null 2>&1
fi

cleanup
trap - EXIT INT TERM

if [ -n "${REBOOT_AFTER:-}" ]; then
    echo "--- $(basename "$DUMP") --- UART log, ${ELAPSED} wall, stopped: ${STALLED} ${REBOOT_AFTER} after 'Starting services...' ---"
else
    echo "--- $(basename "$DUMP") --- UART log, ${ELAPSED} wall, stopped: ${STALLED} ---"
fi
cat "$SERIAL"
echo

if [ "${RAML:-0}" = 1 ]; then
    echo "--- SBL1 in-RAM log copy (early stages only) ---"
    strings -n 8 "$BOOTLOG" | grep -E "^(B|S|D) - |Boot error"
    echo
fi

case $STALLED in
    shell)  exit 0 ;;
    login)  exit 0 ;;
    sbl1-end) exit 0 ;;
    timeout)
        echo "--- did not finish within ${MAXSECS}s ---" >&2
        grep -E "Taking exception|DFAR" /tmp/run.int 2>/dev/null | head -4 >&2
        exit 1
        ;;
    qemu-exited)
        echo "--- QEMU exited early; host messages: ---" >&2
        tail -5 "$ERRLOG" >&2
        exit 1
        ;;
    rebooted)
        echo "--- panic: the image reset itself $REBOOT_AFTER after 'Starting services...' ---" >&2
        echo "--- and booted again from SBL1 (the log above has both cycles) ---" >&2
        exit 0
        ;;
    *)
        echo "--- stopped: $STALLED (the log ends where the image does) ---" >&2
        exit 0
        ;;
esac
