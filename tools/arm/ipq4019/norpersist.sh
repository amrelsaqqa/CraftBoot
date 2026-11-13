#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# norpersist.sh -- write known words to the NOR die from inside OpenWrt, reboot
# the guest, and check the words are still there afterwards.
#
# The die's backing store is the machine's NOR window (plain RAM behind the
# SPI model), so a guest reboot keeps it; a fresh QEMU run starts again from
# the image.  The QSEE bypass is handled by the bypass.sh agent; note that an
# agent armed for one boot does not cover the boot after a reset (see bypass.sh
# and the README), so if guest 2 never appears, re-arm with bypass_reset.
#
# Waits are offset based: every wait only looks at log lines written after the
# point the command was sent, otherwise the first boot's "SBL1, Start" or
# "root@OpenWrt" would satisfy the post-reboot waits.  Markers are matched as
# *whole lines* (`grep -ax`) because the console echoes the command line: the
# literal marker text appears there too, and matching it would mean waiting for
# the echo instead of for the result -- which is how an earlier version of this
# script managed to send its next command into a still-running grep.
set -u
cd ~/qemu-ipq4019
. ./bypass.sh

OUT=/tmp/norpersist
SER=$OUT.ser
FIFO=$OUT.in
GDB=$OUT.gsock
WORDS="IPQPROBE_55AA_CAFE"

rm -f "$SER" "$FIFO" "$GDB"
mkfifo "$FIFO"
./build/qemu-system-arm -M ipq4019 \
    -kernel ~/kt708-ipq-openwrt24.10.0-no-wifi.bin -display none \
    -serial stdio -monitor none -S \
    -gdb "unix:$GDB,server=on,wait=off" < "$FIFO" > "$SER" 2>&1 &
QP=$!
exec 3>"$FIFO"

mark() { wc -l < "$SER"; }

# waitfor_from PATTERN LINE_NO SECS -- substring match, only after LINE_NO
waitfor_from() {
    local pat=$1 from=$2 secs=${3:-60} i
    for ((i = 0; i < secs * 10; i++)); do
        tail -n "+$((from + 1))" "$SER" 2>/dev/null | grep -qa "$pat" && return 0
        sleep 0.1
    done
    return 1
}

# waitline_from LINE LINE_NO SECS -- match a whole output line, never the echo
waitline_from() {
    local want=$1 from=$2 secs=${3:-60} i
    for ((i = 0; i < secs * 10; i++)); do
        tail -n "+$((from + 1))" "$SER" 2>/dev/null | tr -d '\r' |
            grep -qxF "$want" && return 0
        sleep 0.1
    done
    return 1
}

send() { printf '%b' "$1" >&3; }

# run CMD MARKER [SECS] -- send CMD, wait for a line consisting of MARKER
run() {
    local cmd=$1 marker=$2 secs=${3:-60}
    LAST_MARK=$(mark)
    send "$cmd; echo $marker\r"
    waitline_from "$marker" "$LAST_MARK" "$secs"
}

die() { echo "FAIL: $*"; bypass_stop; kill -9 $QP 2>/dev/null; wait $QP 2>/dev/null; exit 1; }

# count MARKER occurrences in the output produced since the last run()
since_run() {
    tail -n "+$((LAST_MARK + 1))" "$SER" | tr '\r' '\n' | grep -ac "$1"
}

sleep 1
bypass_start "$GDB" "$SER" "$OUT.gdb.log" "${QEMU_IPQ_BYPASS:-0xa8d1918:0x80000000}" \
    || die "QSEE bypass"

# ---- boot 1 -------------------------------------------------------------
waitfor_from "urngd: v1.0.2" 0 150 || die "guest 1 never booted"
waitfor_from "switch wan: configuring" 0 120 || die "guest 1: no netifd"
send '\r'
waitfor_from "root@OpenWrt" 0 60 || die "guest 1: no shell"
sleep 1

echo "== writing: $WORDS"
run "echo $WORDS > /overlay/norprobe.txt" MKA-WRITTEN 60 || die "guest 1: write hung"
run "sync" MKA-SYNCED 60 || die "guest 1: sync hung"
run "cat /overlay/norprobe.txt" MKA-READ 30 || die "guest 1: read hung"
seen=$(since_run "$WORDS")
echo "== words read back on guest 1: $seen"

# Force the next read to come off the die instead of the page cache.
run "echo 3 > /proc/sys/vm/drop_caches" MKA-DROPPED 60 || die "guest 1: drop_caches hung"
run "cat /overlay/norprobe.txt" MKA-AGAIN 30 || die "guest 1: read hung"
before=$(since_run "$WORDS")
echo "== words on guest 1 after drop_caches: $before"

# ---- reboot -------------------------------------------------------------
echo "== rebooting the guest"
LAST_MARK=$(mark)
send "sync; reboot -f\r"
waitfor_from "SBL1, Start" "$LAST_MARK" 90 || die "guest did not reset into SBL1"
echo "== SBL1 restarted (the agent must be re-armed for this boot: bypass_reset)"

# ---- boot 2 -------------------------------------------------------------
LAST_MARK=$(mark)
waitfor_from "switch wan: configuring" "$LAST_MARK" 180 || true
send '\r'
waitfor_from "root@OpenWrt" "$LAST_MARK" 120 || die "guest 2: no shell after reboot"
sleep 1

echo "== reading back after reboot"
run "cat /overlay/norprobe.txt" MKB-READ 30 || die "guest 2: readback hung"
after=$(since_run "$WORDS")

# Second opinion straight off the character device, bypassing overlayfs.  This
# is informational: jffs2 compresses, so a raw hit is possible but not required.
raw=?
if run "dd if=/dev/mtd9 bs=4096 count=240 2>/dev/null | grep -ac $WORDS" MKC-RAW 120; then
    raw=$(tail -n "+$((LAST_MARK + 1))" "$SER" | tr '\r' '\n' |
          grep -ax '[0-9]\+' | tail -1)
fi

# The kernel says so loudly if replayed NOR writes were bad: a bad CRC or a
# corrupt node comes out of jffs2 with those words in it.
jffs=?
if run "dmesg | grep -acE 'jffs2.*(rror|CRC|orrupt)'" MKD-JFFS 40; then
    jffs=$(tail -n "+$((LAST_MARK + 1))" "$SER" | tr '\r' '\n' |
           grep -ax '[0-9]\+' | tail -1)
fi

bypass_stop
kill -9 $QP 2>/dev/null
wait $QP 2>/dev/null

boots=$(tr '\r' '\n' < "$SER" | grep -ac "Booting Linux on physical CPU")
echo "== kernel boots seen in the log     : $boots (want 2)"
echo "== words on guest 1, before reboot  : ${before:-0}"
echo "== words after reboot, via overlayfs : ${after:-0}"
echo "== words after reboot, raw on mtd9  : ${raw} (jffs2 may compress)"
echo "== jffs2 error lines in dmesg       : ${jffs} (want 0)"
echo "-- tail of the log --"
tr '\r' '\n' < "$SER" | grep -av '^$' | tail -12

if [ "$boots" -ge 2 ] && [ "${before:-0}" -ge 1 ] && [ "${after:-0}" -ge 1 ]; then
    echo "VERDICT NOR persistence across reboot -> PASS"
else
    echo "VERDICT NOR persistence across reboot -> FAIL"
fi
echo "log: $SER"
