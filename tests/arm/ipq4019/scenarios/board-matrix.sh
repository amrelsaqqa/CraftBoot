#!/bin/bash
# tests/board-matrix.sh -- the regression gate for the SoC/board split.
#
# The refactor that turns this machine into "SoC + configured board" rewrites
# how the external parts are created, but it must not change what the guest
# sees.  This script is the fixed point: a handful of boots that each have to
# reach a named marker, run against whatever build/qemu-system-arm is present.
#
#   ./tests/board-matrix.sh                 # every case it can run
#   ./tests/board-matrix.sh --quick         # just the cold-boot case
#   ./tests/board-matrix.sh --die=/path/nand.bin
#                       a provisioned SPI NAND, so the NAND boot reaches Linux
#                       (copied to a scratch dir first; the original is never
#                       opened writable)
#   ./tests/board-matrix.sh --openwrt=/path/dump.bin
#                       also boot an OpenWrt/Caraboot dump to its U-Boot prompt
#   ./tests/board-matrix.sh --libvirt
#                       also run tests/libvirt-spike.sh: the same machine as a
#                       libvirt domain, described by a board device tree
#   DUMP_DUMP=/path/dump.bin
#                       the full flash dump whose device tree the board presets
#                       are checked against (default: the MikroTik 32M dump)
#
# Nothing here measures time.  Each marker is just "did this appear, and did it
# appear before the deadline"; the deadlines exist so a hang fails instead of
# blocking forever.
set -u
cd "$(dirname "$0")/.." || exit 1

QEMU=./build/qemu-system-arm
NOR=${NOR_DUMP:?set NOR_DUMP to a SPI-NOR dump}
DIE= OPENWRT= QUICK=0 LIBVIRT=0
DUMP=${DUMP_DUMP:-$HOME/mikrotik-dump-latest7.17-32m.bin}
SCRATCH= FAILS=0 PASSES=0

usage() { sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; }

for a in "$@"; do
    case $a in
        --die=*)     DIE=${a#*=} ;;
        --openwrt=*) OPENWRT=${a#*=} ;;
        --libvirt)   LIBVIRT=1 ;;
        --quick)     QUICK=1 ;;
        -h|--help)   usage; exit 0 ;;
        *)           echo "board-matrix: unknown argument $a" >&2; exit 2 ;;
    esac
done

[ -x "$QEMU" ] || { echo "board-matrix: $QEMU is not built" >&2; exit 1; }
[ -r "$NOR" ]  || { echo "board-matrix: no boot dump at $NOR" >&2; exit 1; }

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/board-matrix.XXXXXX") || exit 1
QPIDS=()
cleanup() {
    local p
    for p in "${QPIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
    rm -rf "$SCRATCH"
}
trap cleanup EXIT INT TERM

say()  { printf '%s\n' "$*"; }
pass() { PASSES=$((PASSES + 1)); say "  ok   $*"; }
fail() { FAILS=$((FAILS + 1)); say "  FAIL $*"; }

# wait_count <file> <pattern> <min-lines> <seconds>
wait_count() {
    local f=$1 pat=$2 want=$3 secs=$4 i n
    for ((i = 0; i < secs * 2; i++)); do
        # A log that does not exist yet, or that QEMU has not written to, is
        # "not yet", not a shell error: grep -c prints nothing for a missing
        # file, which turns into an empty string without the ${n:-0}.
        n=$(grep -ac -- "$pat" "$f" 2>/dev/null)
        [ "${n:-0}" -ge "$want" ] && return 0
        sleep 0.5
    done
    return 1
}

wait_for() { wait_count "$1" "$2" 1 "$3"; }

# start <tag> [extra qemu args...] -- sets CASE, CASE_DIR, CASE_LOG, CASE_HOST
#
# Every case gets a SPI NAND, because RouterBOOT writes its config there and a
# missing die changes what the boot does.  The default die is a fresh blank file
# in the case's own directory; NAND_DIE names a provisioned die to copy in
# instead.  Copying, not symlinking, is deliberate: a boot writes to its die and
# the caller's file must come out of a test run unchanged.
NAND_DIE=
start() {
    local tag=$1 die
    shift
    CASE_DIR=$SCRATCH/$tag
    mkdir -p "$CASE_DIR"
    CASE_LOG=$CASE_DIR/console.log
    CASE_HOST=$CASE_DIR/host.log
    die=$CASE_DIR/nand.bin
    if [ -n "$NAND_DIE" ]; then
        die=$CASE_DIR/die.bin
        cp "$NAND_DIE" "$die" || { fail "cannot copy $NAND_DIE"; return 1; }
    fi

    QEMU_IPQ_NAND=1 QEMU_IPQ_NAND_PERSIST=$die \
    "$QEMU" -M ipq4019 -kernel "$NOR" -display none \
        -monitor unix:$CASE_DIR/mon.sock,server=on,wait=off \
        -serial file:"$CASE_LOG" "$@" >"$CASE_HOST" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null    # we reap these by pid; no job-control "Killed" noise
    CASE=$tag
}

# stop -- kill the case's machine (the next case gets its own scratch dir)
stop() {
    local p
    for p in "${QPIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
    QPIDS=()
    sleep 0.2
}

monitor() {
    python3 - "$CASE_DIR/mon.sock" "$@" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall((" ".join(sys.argv[2:]) + "\n").encode())
PY
}

# monitor_read <command> -- like monitor(), but print what the monitor answers
monitor_read() {
    python3 - "$CASE_DIR/mon.sock" "$@" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sys.argv[1])
# Drop the banner the monitor prints when a client connects.
s.settimeout(0.5)
try:
    while s.recv(65536):
        pass
except socket.timeout:
    pass
s.sendall((" ".join(sys.argv[2:]) + "\n").encode())
out = b""
end = time.time() + 5
while time.time() < end:
    try:
        chunk = s.recv(65536)
    except socket.timeout:
        break
    if not chunk:
        break
    out += chunk
    if b"(qemu)" in out:
        break
sys.stdout.write(out.decode(errors="replace"))
PY
}

# --------------------------------------------------------------- cases
# The stage table is read out of the boot image, so its log line is the first
# thing to check: if it is wrong, every address derived from it is wrong.
case_stages() {
    start stages
    if wait_for "$CASE_HOST" "ipq4019: stages:" 20; then
        pass "stage table: $(sed -n 's/^ipq4019: stages: //p' "$CASE_HOST" | head -1)"
    else
        fail "no stage table in host.log"
    fi
    stop
}

# Cold boot: SBL1 runs, the machine skips QSEE at its entry, RouterBOOT starts.
# A blank NAND is enough; this case says nothing about the kernel.
case_cold() {
    start cold
    wait_for "$CASE_HOST" "ipq4019: qsee: skip" 30 \
        && pass "QSEE skipped in-machine (no debugger)" \
        || fail "QSEE handover was not intercepted"
    wait_for "$CASE_LOG" "RouterBOOT booter" 60 \
        && pass "RouterBOOT reached" \
        || fail "RouterBOOT never started"
    stop
}

# A system reset must run the same chain again: the breakpoint is machine
# state, not guest memory, and the reset drops the previous boot's I-cache.
case_reset() {
    start reset
    wait_for "$CASE_HOST" "ipq4019: qsee: skip" 30 \
        && pass "first boot skipped QSEE" \
        || fail "first boot did not skip QSEE"
    monitor system_reset
    if wait_count "$CASE_HOST" "ipq4019: qsee: skip" 2 60; then
        pass "after system_reset the handover was skipped again"
    else
        fail "system_reset did not reach a second QSEE skip"
    fi
    wait_count "$CASE_LOG" "RouterBOOT booter" 2 90 \
        && pass "second RouterBOOT banner" \
        || fail "second boot never reached RouterBOOT"
    stop
}

# A provisioned NAND must carry RouterBOOT all the way to Linux: that is the
# part of the boot that depends on the die contents, not just on the controller.
case_nand_kernel() {
    [ -n "$DIE" ] || { say "  skip nand-kernel (no --die=)"; return; }
    NAND_DIE=$DIE start nand-kernel
    NAND_DIE=
    wait_for "$CASE_HOST" "ipq4019: qsee: skip" 30 \
        && pass "QSEE skipped in-machine" \
        || fail "QSEE handover was not intercepted"
    wait_for "$CASE_LOG" "Booting Linux" 150 \
        && pass "NAND boot reached the kernel" \
        || fail "NAND boot did not reach the kernel"
    stop
}

# The same board, spelled out instead of taken from a preset: board=none has
# no die at all, so this also proves that the explicit properties and the
# compiled-in preset reach the machine by the same path.
case_explicit_board() {
    start cfg-explicit -M ipq4019,board=none,nor=on,nor-cs-gpio=54
    wait_for "$CASE_HOST" "ipq4019: board none: nor 32 MiB" 20 \
        && pass "board=none + nor=on resolves to a 32 MiB NOR" \
        || fail "board=none + nor=on did not resolve"
    wait_for "$CASE_LOG" "RouterBOOT booter" 60 \
        && pass "explicit board booted to RouterBOOT" \
        || fail "explicit board never reached RouterBOOT"
    stop
}

# The chip select pin is board wiring, so it has to travel in the board
# configuration.  (It is not a disconnect test: the boot chain selects the die
# with the QUP's own chip select, and only the GPIO path -- the one Linux uses
# -- goes through the pin.  That is why both paths stay modelled.)
case_cs_pin() {
    start cfg-cs -M ipq4019,nor-cs-gpio=60
    wait_for "$CASE_HOST" "ipq4019: board default: nor 32 MiB cs gpio 60" 20 \
        && pass "nor-cs-gpio=60 reaches the board configuration" \
        || fail "nor-cs-gpio did not reach the board configuration"
    stop
}

# The die can be a block backend instead of a path the machine opens itself,
# which is what a libvirt <disk> becomes.  The empty store is programmed from
# the boot image on the first run; the second run has no -kernel at all, so the
# only thing that can carry SBL1 into it is the block backend.
case_drive() {
    local dir=$SCRATCH/drive img=$SCRATCH/drive/nor.img

    mkdir -p "$dir"
    : > "$img"
    start drive1 -M ipq4019,nor-drive=nor \
        -drive if=none,id=nor,file=$img,format=raw
    wait_for "$CASE_HOST" "ipq4019: nor store .* written" 20 \
        && pass "a -drive backend is programmed from the boot image" \
        || fail "the -drive backend was never programmed"
    stop

    if [ "$(stat -c %s "$img")" -lt 33554432 ] 2>/dev/null; then
        fail "the -drive backend is $(stat -c %s "$img") bytes, not a die"
        return
    fi

    "$QEMU" -M ipq4019,nor-drive=nor \
        -drive if=none,id=nor,file=$img,format=raw \
        -display none -monitor none -serial file:"$dir/console2.log" \
        >"$dir/host2.log" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null
    wait_for "$dir/host2.log" "ipq4019: nor store .* loaded" 20 \
        && pass "the same backend is loaded on a second run" \
        || fail "the -drive backend was not loaded on a second run"
    wait_for "$dir/console2.log" "RouterBOOT booter" 90 \
        && pass "the second run boots with no -kernel: the store is the board" \
        || fail "the second run did not boot from the store"
    stop
}

# A backend opened read-only is a write-protected part, not a broken machine.
case_drive_ro() {
    local dir=$SCRATCH/drive-ro img=$SCRATCH/drive-ro/nor.img

    mkdir -p "$dir"
    cp "$SCRATCH/drive/nor.img" "$img" 2>/dev/null
    [ -s "$img" ] || { say "  skip drive-ro (no store from case_drive)"; return; }
    "$QEMU" -M ipq4019,nor-drive=nor \
        -drive if=none,id=nor,file=$img,format=raw,readonly=on \
        -display none -monitor none -serial null >"$dir/host.log" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null
    wait_for "$dir/host.log" "read-only.*refuse program and erase" 20 \
        && pass "a read-only backend is reported as a write-protected die" \
        || fail "a read-only backend was not reported"
    stop
}

# -kernel is a programming source, never a boot source: the boot chain comes
# out of the die.  Three consequences, and all three are worth a line in the
# log, because "I passed a dump and booted something else" is exactly the kind
# of quiet difference this machine is supposed to make impossible: an image
# handed to a die that already holds a chain is not programmed (and says so),
# a die that holds no chain and no image is refused instead of running an
# empty boot ROM, and a blank part still gets programmed from the image.
case_boot_source() {
    local dir=$SCRATCH/bootsrc die=$SCRATCH/bootsrc/die.img
    local blank=$SCRATCH/bootsrc/erased.img zero=$SCRATCH/bootsrc/zero.img out

    mkdir -p "$dir"
    cp "$SCRATCH/drive/nor.img" "$die" 2>/dev/null
    [ -s "$die" ] || { say "  skip boot-source (no store from case_drive)"; return; }

    "$QEMU" -M ipq4019,nor-file=$die -kernel "$NOR" -display none \
        -monitor none -serial file:"$dir/console1.log" \
        >"$dir/host1.log" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null
    wait_for "$dir/host1.log" "already holds a boot chain" 20 \
        && pass "an image is not programmed over a die that already has one" \
        || fail "the machine did not say the image could not be used"
    wait_for "$dir/console1.log" "RouterBOOT booter" 60 \
        && pass "and the die's own boot chain is what runs" \
        || fail "the die did not boot"
    stop

    python3 -c "open('$blank', 'wb').write(b'\\xff' * 33554432)"
    out=$(timeout 20 "$QEMU" -M ipq4019,nor-file=$blank -display none \
          -monitor none -serial null 2>&1)
    [ $? -ne 0 ] && echo "$out" | grep -q "holds no SBL ELF" \
        && pass "a die with no boot chain and no image is refused" \
        || fail "a die with no boot chain was booted anyway"
    out=$(timeout 20 "$QEMU" -M ipq4019,nor-file=$blank -kernel "$NOR" \
          -display none -monitor none -serial null 2>&1)
    echo "$out" | grep -q -- "-kernel .* is a programming source" \
        && pass "and -kernel is named as the programmer it is" \
        || fail "-kernel was not explained when it could not be used"

    truncate -s 33554432 "$zero"
    "$QEMU" -M ipq4019,nor-file=$zero -kernel "$NOR" -display none \
        -monitor none -serial file:"$dir/console2.log" >"$dir/host2.log" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null
    wait_for "$dir/host2.log" "is blank (all 0x00).*programming it from" 20 \
        && pass "a blank part is still programmed from the image" \
        || fail "a blank part was not programmed"
    wait_for "$dir/console2.log" "RouterBOOT booter" 60 \
        && pass "then boots from the die it just programmed" \
        || fail "the programmed part did not boot"
    stop
}

# A boot strap that names a device the board does not have must be refused,
# not silently booted from whatever happens to be there.
# The board's LEDs are a display console: one labelled lamp per LED, and each
# lamp shows the level the guest drives.  Nothing has booted here, so every pin
# is still at its reset level -- an active-low lamp is dark and an active-high
# one is lit -- and the panel has to show exactly that.  QMP's screendump reads
# the console surface, so this checks the pixels with no window at all.
case_leds() {
    local dir=$SCRATCH/leds ppm=$SCRATCH/leds/panel.ppm

    mkdir -p "$dir"
    "$QEMU" -M 'ipq4019,leds=status:5:al;lan:7;wps:8:al' -display none \
        -monitor unix:$dir/mon.sock,server=on,wait=off -serial null \
        >"$dir/host.log" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null
    wait_for "$dir/host.log" "ipq4019: leds: status@5(al) lan@7 wps@8(al)" 20 \
        && pass "the LEDs are bound to their pins" \
        || fail "the LED binding is not in the board log"

    python3 - "$dir/mon.sock" "$ppm" <<'PY2' || true
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(("screendump %s\n" % sys.argv[2]).encode())
time.sleep(1)
PY2

    if [ ! -s "$ppm" ]; then
        fail "the LED panel produced no screendump"
        stop
        return
    fi
    python3 - "$ppm" <<'PY2' && pass "the panel draws an on lamp, an off lamp and the labels" \
                              || fail "the panel pixels are not the expected lamps"
import sys
d = open(sys.argv[1], "rb").read()
head = d.split(b"\n", 3)
assert head[0] == b"P6", head[0]
w, h = map(int, head[1].split())
assert (w, h) == (128, 80), (w, h)
px = head[3]
def n(rgb):
    r, g, b = rgb
    return sum(1 for i in range(0, len(px), 3)
               if px[i] == r and px[i + 1] == g and px[i + 2] == b)
on  = n((0x38, 0xe0, 0x70))     # an active-high pin released: lit
off = n((0x1c, 0x28, 0x30))     # an active-low pin released: dark
text = n((0xa8, 0xb4, 0xc0))
assert on > 50 and off > 100 and text > 200, (on, off, text)
PY2
    stop
}

# A key is a pin level, so pressing it is not something the guest has to be
# ready for: QMP's send-key (through the HMP monitor here) is the same event
# path a keystroke in the window takes.
case_keys() {
    start keys -M 'ipq4019,keys=sw1:60:al:esc;sw2:61:al:f'
    wait_for "$CASE_HOST" "ipq4019: keys: sw1@60(al)->esc sw2@61(al)->f" 20 \
        && pass "the keys are bound to their pins and qcodes" \
        || fail "the key binding is not in the board log"

    monitor sendkey esc
    wait_for "$CASE_HOST" "ipq4019: key sw1 pressed (gpio 60, level 0)" 10 \
        && pass "sendkey presses the key: the pin reads low" \
        || fail "the key press did not reach the pin"
    wait_for "$CASE_HOST" "ipq4019: key sw1 released (gpio 60, level 1)" 10 \
        && pass "releasing the key returns the pin high" \
        || fail "the key release did not reach the pin"
    stop
}

# The switch is board data too.  Which PHYs answer on the MDIO bus, what they
# identify as, and which port the host's -netdev is wired to are properties, and
# the machine prints the bus it built: no guest has to run for this, so the case
# is a configuration check, not a boot.
case_mdio() {
    local dir=$SCRATCH/mdio

    mkdir -p "$dir"
    "$QEMU" -M ipq4019 -display none -monitor none -serial null \
        >"$dir/default.log" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null
    wait_for "$dir/default.log" \
        "ipq4019: mdio@00090000, 6 port(s): 0@1000 1@1000 2@1000 3@1000 4@1000 5@1000" 20 \
        && pass "the default board puts the QCA8075's six MDIO addresses on the bus" \
        || fail "the default MDIO population is not the QCA8075's six addresses"
    stop

    "$QEMU" -M 'ipq4019,eth-ports=0;1;5!' -display none -monitor none \
        -serial null >"$dir/fewer.log" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null
    wait_for "$dir/fewer.log" \
        "ipq4019: mdio@00090000, 3 port(s): 0@1000 1@1000 5@1000(down)" 20 \
        && pass "eth-ports=0;1;5! builds a three-PHY bus, port 5 unplugged" \
        || fail "eth-ports=0;1;5! did not reach the MDIO bus"
    stop

    "$QEMU" -M 'ipq4019,eth-ports=2:100;3' -display none -monitor none \
        -serial null >"$dir/speed.log" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null
    wait_for "$dir/speed.log" "ipq4019: mdio@00090000, 3 port(s): 2@100 3@1000 5@1000" 20 \
        && pass "a port can name its own speed" \
        || fail "a port's own speed did not reach the MDIO bus"
    stop

    "$QEMU" -M 'ipq4019,eth-ports=' -display none -monitor none \
        -serial null >"$dir/empty.log" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null
    wait_for "$dir/empty.log" "ipq4019: mdio@00090000, no port" 20 \
        && pass "eth-ports= is a board with no switch at all" \
        || fail "an empty eth-ports= still put a switch on the bus"
    stop

    local out
    out=$(timeout 20 "$QEMU" -M ipq4019,eth-bridge-port=9 -display none \
          -monitor none -serial null 2>&1)
    grep -q "eth-bridge-port=9 must be 1..6" <<<"$out" \
        && pass "eth-bridge-port=9 is refused: $(sed 's/.*ipq4019: //' <<<"$out")" \
        || fail "eth-bridge-port=9 was accepted"
}

# The switch's device id is programmed while the machine is built, and the
# machine resets itself once before the guest's first instruction.  A reset
# that zeroed register files instead of restoring their power-on contents drops
# that id; qca8k then refuses to bind ("Switch id detected 0 but expected 14")
# and an OpenWrt guest never gets lan1..4/wan.  The machine is paused, so what
# the monitor reads is exactly the state the guest's first instruction sees.
case_ess() {
    local v i

    start ess -S
    for ((i = 0; i < 10; i++)); do
        [ -S "$CASE_DIR/mon.sock" ] && break
        sleep 0.5
    done
    v=$(monitor_read "xp /1xw 0x0c000000")
    case $v in
        *0x00001402*)
            pass "the switch still reports its id 0x14 after the power-on reset" ;;
        *)
            fail "the switch id did not survive the power-on reset: ${v%%$'\n'*}" ;;
    esac
    stop
}

# The board's device tree is the description of the board, and tools/dts2xml.py
# is what turns it into -machine properties.  That translation is checked
# against a tree that was not written for it -- the one inside the MikroTik
# dump -- by tests/dts2xml-test.sh, which needs no QEMU at all.
case_dts2xml() {
    local out summary
    out=$(timeout 180 ./tests/dts2xml-test.sh 2>&1)
    summary=$(sed -n 's/^\([0-9]* passed, [0-9]* failed\)$/\1/p' <<<"$out" | tail -1)
    if [ -n "$summary" ] && has_zero_failures "$summary"; then
        pass "dts2xml: $summary against the MikroTik device tree"
    else
        fail "dts2xml: ${summary:-the test did not finish}"
        sed 's/^/       /' <<<"$out" | tail -12
    fi
}

has_zero_failures() {
    case $1 in
        *" 0 failed") return 0 ;;
        *) return 1 ;;
    esac
}

# libvirt is optional: it needs a daemon and a policy that lets a home-built
# QEMU run, so the spike is a separate script and this is only the wire-in.
case_libvirt() {
    local out summary
    out=$(timeout 300 ./tests/libvirt-spike.sh 2>&1)
    summary=$(sed -n 's/^\([0-9]* passed, [0-9]* failed\)$/\1/p' <<<"$out" | tail -1)
    if [ -n "$summary" ] && has_zero_failures "$summary"; then
        pass "libvirt: $summary as a domain described by the board tree"
    else
        fail "libvirt: ${summary:-the spike did not finish}"
        sed 's/^/       /' <<<"$out" | tail -12
    fi
}

# The lamps are a display console, so a display backend shows them.  This
# build is headless; when a second build with VNC exists (see the README), the
# same board is watched through it with tools/panel-vnc.py -- a raw RFB client,
# because no viewer is installed here.  The pixels have to match what QMP's
# screendump shows, so the check is the same on both paths.
VNC_QEMU=./build-ui/qemu-system-arm
case_vnc() {
    local dir=$SCRATCH/vnc display=77 port=5977 ppm=$SCRATCH/vnc/panel.png

    # `-vnc help` lists the suboptions; a build without VNC calls it an
    # invalid option, and it exits 1 either way, so the text is the probe.
    if [ ! -x "$VNC_QEMU" ] \
       || ! "$VNC_QEMU" -vnc help 2>&1 | grep -q "^vnc options:"; then
        say "  skip vnc (no VNC-enabled build at $VNC_QEMU)"
        return
    fi
    python3 -c 'import PIL' 2>/dev/null \
        || { say "  skip vnc (no Pillow to read the capture)"; return; }
    mkdir -p "$dir"
    # -vnc host:n takes a *display* number: the port is 5900+n.
    "$VNC_QEMU" -M 'ipq4019,leds=status:5:al;lan:7;wps:8:al' \
        -vnc 127.0.0.1:$display -monitor none -serial null \
        >"$dir/host.log" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null
    wait_for "$dir/host.log" "ipq4019: leds: status@5(al) lan@7 wps@8(al)" 20 \
        && pass "the VNC build wires the same three lamps" \
        || fail "the VNC build did not reach the board"

    if python3 ./tools/panel-vnc.py 127.0.0.1:$port --out "$ppm" \
            >"$dir/capture.log" 2>&1; then
        pass "a VNC client captured the panel: $(cat "$dir/capture.log")"
    else
        fail "the VNC capture failed"
        sed 's/^/       /' "$dir/capture.log"
        stop
        return
    fi

    python3 - "$ppm" <<'PY2' && pass "the VNC image is the same panel: size, lamps and labels" \
                              || fail "the VNC image is not the panel"
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
assert im.size == (128, 80), im.size
px = list(im.getdata())
def n(rgb):
    return sum(1 for p in px if p == rgb)
on   = n((0x38, 0xe0, 0x70))
off  = n((0x1c, 0x28, 0x30))
text = n((0xa8, 0xb4, 0xc0))
assert on > 50 and off > 100 and text > 200, (on, off, text)
PY2
    stop
}

# A preset is a compiled-in board: it fills the same struct an explicit
# property would, and the properties still win.  `mikrotik-952` is not invented
# -- every fact in it is read from that board's own device tree -- so the case
# also checks the preset against what tools/dts2xml.py makes of that tree.
case_preset() {
    local dir=$SCRATCH/preset dtb=$SCRATCH/preset/board.dtb

    mkdir -p "$dir"

    timeout 25 "$QEMU" -M ipq4019,board=mikrotik-952 -kernel "$NOR" \
        -display none -monitor none -serial file:"$dir/console.log" \
        >"$dir/host.log" 2>&1
    wait_for "$dir/console.log" "RouterBOOT" 20 \
        && pass "board=mikrotik-952 boots the vendor dump to RouterBOOT" \
        || fail "the mikrotik-952 preset did not reach RouterBOOT"

    wait_for "$dir/host.log" \
        "ipq4019: board mikrotik-952: nor 32 MiB cs gpio 54, nand 132 MiB cs gpio 59, qpic-nand none, boot-device spi-nor" 5 \
        && pass "the preset carries both dies and the boot strap" \
        || fail "the preset's dies are wrong: $(grep -a 'ipq4019: board ' "$dir/host.log" | head -1)"

    wait_for "$dir/host.log" \
        "ipq4019: leds: user-led@3 power-led@0 usb-power-off@2(al)" 5 \
        && pass "the preset's lamps are the tree's three lines, polarity included" \
        || fail "the preset's lamps are wrong"

    wait_for "$dir/host.log" \
        "ipq4019: mdio@00090000, 6 port(s): 0@1000 1@1000 2@1000 3@1000 4@1000 5@1000" 5 \
        && pass "the preset's switch is the tree's five PHYs plus its PSGMII block" \
        || fail "the preset's MDIO population is wrong"

    timeout 15 "$QEMU" -M 'ipq4019,board=mikrotik-952,leds=only:6,nor=off,boot-device=none' \
        -display none -monitor none -serial null >"$dir/override.log" 2>&1
    wait_for "$dir/override.log" "ipq4019: leds: only@6" 5 \
        && pass "an explicit leds= replaces the preset's panel" \
        || fail "the preset's lamps were not overridable"
    wait_for "$dir/override.log" "nor none" 5 \
        && pass "an explicit nor=off removes the preset's NOR" \
        || fail "the preset's NOR was not overridable"

    # The same board, described by its own tree: what the machine prints for the
    # preset and what the converter derives from the DTB have to agree.
    python3 - "$DUMP" "$dtb" <<'PY2'
import struct, sys
data = open(sys.argv[1], "rb").read()
i = 0
while True:
    i = data.find(b"\xd0\x0d\xfe\xed", i)
    if i < 0:
        sys.exit("no device tree in %s" % sys.argv[1])
    total = struct.unpack(">I", data[i + 4:i + 8])[0]
    if 4096 < total <= len(data) - i:
        open(sys.argv[2], "wb").write(data[i:i + total])
        break
    i += 4
PY2
    local props
    props=$(./tools/dts2xml.py "$dtb" --print-machine 2>/dev/null)
    case $props in
        *"eth-ports=0;1;2;3;4"*)
            pass "the converter reads the same five PHYs out of the tree" ;;
        *)  fail "the converter and the preset disagree about the switch: $props" ;;
    esac
    case $props in
        *"leds=user-led:3;power-led:0;usb-power-off:2:al"*)
            pass "the converter reads the same lamps out of the tree" ;;
        *)  fail "the converter and the preset disagree about the lamps: $props" ;;
    esac
}

case_refuse() {
    local out
    out=$(timeout 20 "$QEMU" -M ipq4019,nor=off -display none -monitor none \
          -serial null 2>&1)
    [ $? -ne 0 ] && echo "$out" | grep -q "no NOR die is configured" \
        && pass "nor=off is refused: $(echo "$out" | sed 's/.*ipq4019: //')" \
        || fail "nor=off was not refused (or booted anyway)"
}

# The OpenWrt/Caraboot dumps have U-Boot at 0x87300000 instead of RouterBOOT;
# the stage table has to find that entry too, with no script change.
case_openwrt() {
    [ -n "$OPENWRT" ] || { say "  skip openwrt (no --openwrt=)"; return; }
    local dir=$SCRATCH/openwrt
    mkdir -p "$dir"
    "$QEMU" -M ipq4019 -kernel "$OPENWRT" -display none -monitor none \
        -serial file:"$dir/console.log" >"$dir/host.log" 2>&1 &
    QPIDS+=($!)
    disown $! 2>/dev/null
    wait_for "$dir/host.log" "ipq4019: stages:" 20 \
        && pass "stage table: $(sed -n 's/^ipq4019: stages: //p' "$dir/host.log" | head -1)" \
        || fail "no stage table for the OpenWrt dump"
    wait_for "$dir/console.log" "U-Boot" 60 \
        && pass "U-Boot reached" \
        || fail "U-Boot never started"
    stop
}

# The parallel NAND behind the QPIC controller is a second bus, a second BAM
# and a second boot strap, so it gets its own case: the preset has to describe
# the tree's die, a die that is not whole erase blocks is refused, the strap
# cannot name a die that is not there, and RouterBOOT has to reach the part.
case_qpic() {
    local dir=$SCRATCH/qpic
    mkdir -p "$dir"

    timeout 15 "$QEMU" -M ipq4019,board=hap-ac3 -display none -monitor none \
        -serial null >"$dir/preset.log" 2>&1
    wait_for "$dir/preset.log" \
        "qpic-nand 132 MiB, id c2 f1 80 1d, 65536 pages of 2048+64, 1024 blocks" 5 \
        && pass "board=hap-ac3 carries the tree's parallel NAND" \
        || fail "the hAP ac3 preset has no QPIC die"

    # Geometry: a die is a whole number of 132 KiB erase blocks, nothing else.
    timeout 10 "$QEMU" -M ipq4019,board=none,qpic-nand=on,qpic-nand-size=1M \
        -display none -monitor none -serial null >"$dir/size.log" 2>&1
    [ $? -ne 0 ] && grep -q "whole number of" "$dir/size.log" \
        && pass "qpic-nand-size refuses a partial erase block" \
        || fail "a partial erase block was accepted as a die"

    # The strap has to name a die that exists.
    timeout 10 "$QEMU" -M ipq4019,board=none,boot-device=qpic-nand \
        -display none -monitor none -serial null >"$dir/boot.log" 2>&1
    [ $? -ne 0 ] && grep -q "no parallel NAND die is configured" "$dir/boot.log" \
        && pass "boot-device=qpic-nand without a die is refused" \
        || fail "the QPIC boot strap was accepted with no die"

    # The store is the part: an empty file becomes a die, and the next run
    # reads it back instead of creating one.
    : > "$dir/die.bin"
    timeout 15 "$QEMU" -M "ipq4019,board=hap-ac3,qpic-nand-size=132K,qpic-nand-file=$dir/die.bin" \
        -display none -monitor none -serial null >"$dir/fresh.log" 2>&1
    [ "$(stat -c %s "$dir/die.bin" 2>/dev/null)" = 135168 ] \
        && pass "an empty qpic-nand-file becomes one 132 KiB erase block" \
        || fail "the qpic die store was not sized to the part"
    timeout 15 "$QEMU" -M "ipq4019,board=hap-ac3,qpic-nand-size=132K,qpic-nand-file=$dir/die.bin" \
        -display none -monitor none -serial null >"$dir/loaded.log" 2>&1
    wait_for "$dir/loaded.log" "qpic-nand 135168 bytes, id c2 f1 80 1d, 64 pages of 2048+64, 1 blocks <- the store" 5 \
        && pass "the die file is the part on the next power-on" \
        || fail "the qpic die file was not loaded as the part"

    # And the part answers the vendor protocol.  RouterBOOT would prove that,
    # but the NOR dump in this tree (7.18.2) is a 952 build: it finds the SPI
    # NAND on chip select 1 and never issues a QPIC command, so its log says
    # nothing about this die.  Instead a stub drives NANDc directly the way the
    # mainline driver does -- FETCH_ID, then a page read at page 32 -- and
    # prints what came back over UART0.  The die is one erase block with a
    # known marker at page 32, so a wrong cursor or a wrong page is visible.
    python3 - "$dir" <<'PY'
import struct, sys

def elf(words, path, entry=0x80008000):
    off, blob = 0x100, struct.pack('<%dI' % len(words), *words)
    ehdr = b'\x7fELF' + bytes([1, 1, 1, 0]) + bytes(8)
    ehdr += struct.pack('<HHIIIIIHHHHHH', 2, 40, 1, entry, 52, 0,
                        0x05000000, 52, 32, 1, 0, 0, 0)
    phdr = struct.pack('<IIIIIIII', 1, off, entry, entry, len(blob), len(blob),
                       5, 0x1000)
    open(path, 'wb').write(ehdr + phdr + b'\0' * (off - 84) + blob)

CONST = [
    0x079b0000,     # 0 NANDc
    0x078af000,     # 1 BLSP1 UART0 (a byte store to +0x100 goes on the wire)
]

# 'i','d','=' -> one mov + one strb per character
def text(s, uart=4):
    return [ins for ch in s for ins in (('mov', 1, ord(ch)), ('strb', 1, uart, 0x100))]

# NANDc register offsets (include/linux/mtd/nand-qpic-common.h)
CMD, ADDR0, EXEC, READ_ID, BUF = 0x000, 0x004, 0x010, 0x040, 0x100
FETCH_ID, PAGE_READ = 0x0b, 0x32        # 0x32 = PAGE_READ | PAGE_ACC | LAST_PAGE
BLOCK_ERASE = 0x0a

prog = (
    [('ldr_lit', 0, 0), ('ldr_lit', 4, 1),
     ('mov', 1, FETCH_ID), ('str', 1, 0, CMD),
     ('mov', 1, 1),        ('str', 1, 0, EXEC),
     ('ldr', 2, 0, READ_ID), ('rev', 2, 2)]
    + text('id=')
    + [('bl', 'hex')]
    + text(' page=')
    + [('mov', 1, 0x20), ('lsl', 1, 1, 16),          # page 32
       ('str', 1, 0, ADDR0),
       ('mov', 1, PAGE_READ), ('str', 1, 0, CMD),
       ('mov', 1, 1),         ('str', 1, 0, EXEC),
       # the page buffer decodes bytewise, so collect four bytes into a word
       ('ldrb', 2, 0, BUF + 0),
       ('ldrb', 1, 0, BUF + 1), ('lsl', 1, 1, 8),  ('orr', 2, 2, 1),
       ('ldrb', 1, 0, BUF + 2), ('lsl', 1, 1, 16), ('orr', 2, 2, 1),
       ('ldrb', 1, 0, BUF + 3), ('lsl', 1, 1, 24), ('orr', 2, 2, 1),
       ('rev', 2, 2), ('bl', 'hex')]
    # Erase the block the marker is in and read the page back: the array
    # changed, so the controller must not answer from the page buffer it was
    # holding, and the store has to hold the erase once this exits.
    + text(' erased=')
    + [('mov', 1, BLOCK_ERASE), ('str', 1, 0, CMD),
       ('mov', 1, 1),           ('str', 1, 0, EXEC),
       ('mov', 1, PAGE_READ),   ('str', 1, 0, CMD),
       ('mov', 1, 1),           ('str', 1, 0, EXEC),
       ('ldrb', 2, 0, BUF + 0),
       ('ldrb', 1, 0, BUF + 1), ('lsl', 1, 1, 8),  ('orr', 2, 2, 1),
       ('ldrb', 1, 0, BUF + 2), ('lsl', 1, 1, 16), ('orr', 2, 2, 1),
       ('ldrb', 1, 0, BUF + 3), ('lsl', 1, 1, 24), ('orr', 2, 2, 1),
       ('rev', 2, 2), ('bl', 'hex')]
    + text('\r\n')
    + [('b', '.'),
       ('label', 'hex'),                            # print r2 as 8 hex digits
       ('mov', 3, 8), ('label', 'hexloop'),
       ('lsr', 1, 2, 28), ('and', 1, 1, 0xf), ('cmp', 1, 9),
       ('add', 1, 1, ord('0')), ('addgt', 1, 1, ord('a') - ord('0') - 10),
       ('strb', 1, 4, 0x100), ('lsl', 2, 2, 4), ('subs', 3, 3, 1),
       ('bne', 'hexloop'), ('bx',)]
)

# A literal load is PC+relative and a branch is relative to PC+8, so both need
# the whole list (and the constant pool behind it) before they can be encoded.
# Labels are positions, not instructions: they take no word of their own.
labels, body = {}, []
for ins in prog:
    if ins[0] == 'label':
        labels[ins[1]] = len(body) * 4
    else:
        body.append(ins)
pool = len(body) * 4

words = []
for i, ins in enumerate(body):
    op, pc = ins[0], i * 4
    if op == 'ldr_lit':
        words.append(0xe59f0000 | (ins[1] << 12) |
                     (pool + 4 * ins[2] - (pc + 8)))
    elif op == 'mov':
        words.append(0xe3a00000 | (ins[1] << 12) | ins[2])
    elif op == 'add':
        words.append(0xe2800000 | (ins[2] << 16) | (ins[1] << 12) | ins[3])
    elif op == 'addgt':
        words.append(0xc2800000 | (ins[2] << 16) | (ins[1] << 12) | ins[3])
    elif op == 'and':
        words.append(0xe2000000 | (ins[2] << 16) | (ins[1] << 12) | ins[3])
    elif op == 'cmp':
        words.append(0xe3500000 | (ins[1] << 16) | ins[2])
    elif op == 'subs':
        words.append(0xe2500000 | (ins[2] << 16) | (ins[1] << 12) | ins[3])
    elif op == 'lsl':
        words.append(0xe1a00000 | (ins[1] << 12) | (ins[3] << 7) | ins[2])
    elif op == 'lsr':
        words.append(0xe1a00020 | (ins[1] << 12) | (ins[3] << 7) | ins[2])
    elif op == 'str':
        words.append(0xe5800000 | (ins[2] << 16) | (ins[1] << 12) | ins[3])
    elif op == 'strb':
        words.append(0xe5c00000 | (ins[2] << 16) | (ins[1] << 12) | ins[3])
    elif op == 'ldr':
        words.append(0xe5900000 | (ins[2] << 16) | (ins[1] << 12) | ins[3])
    elif op == 'ldrb':
        words.append(0xe5d00000 | (ins[2] << 16) | (ins[1] << 12) | ins[3])
    elif op == 'orr':
        words.append(0xe1800000 | (ins[2] << 16) | (ins[1] << 12) | ins[3])
    elif op == 'rev':
        words.append(0xe6bf0f30 | (ins[1] << 12) | ins[2])
    elif op == 'bl':
        words.append(0xeb000000 |
                     (((labels[ins[1]] - (pc + 8)) >> 2) & 0xffffff))
    elif op == 'bne':
        words.append(0x1a000000 |
                     (((labels[ins[1]] - (pc + 8)) >> 2) & 0xffffff))
    elif op == 'b':
        words.append(0xeafffffe)
    elif op == 'bx':
        words.append(0xe12fff1e)
    else:
        raise SystemExit('qpic test: unhandled ' + op)

elf(words + CONST, sys.argv[1] + '/qpic-id.elf')

# One erase block, erased, with a marker at page 32 that only the right page
# read can produce.
die = bytearray(b'\xff' * (64 * 2112))
die[32 * 2112:32 * 2112 + 4] = b'QPIC'
open(sys.argv[1] + '/qpic-die.bin', 'wb').write(die)
PY
    timeout 20 "$QEMU" -M "ipq4019,board=none,nor=on,nor-size=1M,qpic-nand=on,qpic-nand-size=132K,qpic-nand-file=$dir/qpic-die.bin" \
        -kernel "$dir/qpic-id.elf" -display none -monitor none \
        -serial file:"$dir/qpic-console.log" >"$dir/qpic-stub.log" 2>&1
    wait_for "$dir/qpic-console.log" "id=c2f1801d page=51504943 erased=ffffffff" 10 \
        && pass "NANDc answers FETCH_ID, a page read and an erase (c2 f1 80 1d, 'QPIC')" \
        || fail "the QPIC die did not answer: $(tr -d '\r' < "$dir/qpic-console.log" | tail -1)"
    python3 -c "import sys; d=open('$dir/qpic-die.bin','rb').read(); \
                sys.exit(0 if d == b'\\xff' * 135168 else 1)" \
        && pass "the erase reached the store: the die file is erased" \
        || fail "the die file still holds the marker after an erase"

    # The other half of the contract: a blank store is programmed from -kernel,
    # and the boot chain is then read back *through* the controller (that is
    # what strips the spare), so the same stub runs out of the die it was
    # written into.
    : > "$dir/prog-die.bin"
    timeout 10 "$QEMU" -M "ipq4019,board=none,nor=on,nor-size=1M,qpic-nand=on,qpic-nand-size=132K,qpic-nand-file=$dir/prog-die.bin,boot-device=qpic-nand" \
        -kernel "$dir/qpic-id.elf" -display none -monitor none \
        -serial file:"$dir/prog-console.log" >"$dir/prog-host.log" 2>&1
    wait_for "$dir/prog-host.log" "qpic-nand die programmed from" 5 \
        && pass "a blank QPIC store is programmed from -kernel" \
        || fail "the QPIC store was not programmed from the image"
    wait_for "$dir/prog-host.log" "loaded the QPIC NAND die (SBL ELF)" 5 \
        && pass "the programmed die is read back through NANDc, spare stripped" \
        || fail "the programmed QPIC die was not readable as a boot chain"
}

# `/system shutdown` has to end the machine.
#
# This SoC has no power-off register, and MikroTik's kernel is given only
# `qcom,pshold` (a restart handler), so kernel_power_off() prints
# "reboot: Power down" and arch/arm's default machine_halt() masks IRQs and
# spins on WFI.  The machine reads that spin as the plug coming out; see
# ipq_poweroff_poll().  Testing it on RouterOS costs a 40 s boot, so the case
# uses two stubs instead: one that raises UART0's interrupt while IRQs are
# unmasked, masks them, and then WFI-spins (a real pending line, exactly the
# state a hung shutdown shows), and one that only masks and spins (not a
# power-off: nothing is pending, WFI really halts).
case_poweroff() {
    local dir=$SCRATCH/poweroff
    mkdir -p "$dir"
    python3 - "$dir" <<'PY'
import struct, sys

def elf(words, path, entry=0x80008000):
    off, blob = 0x100, struct.pack('<%dI' % len(words), *words)
    ehdr = b'\x7fELF' + bytes([1, 1, 1, 0]) + bytes(8)
    ehdr += struct.pack('<HHIIIIIHHHHHH', 2, 40, 1, entry, 52, 0,
                        0x05000000, 52, 32, 1, 0, 0, 0)
    phdr = struct.pack('<IIIIIIII', 1, off, entry, entry, len(blob), len(blob),
                       5, 0x1000)
    open(path, 'wb').write(ehdr + phdr + b'\0' * (off - 84) + blob)

CONST = [
    0x0b000000,     # 0 GICD (the distributor; QGIC2's CPU interface is +0x2000)
    0x0b002000,     # 1 GICC
    0x078af000,     # 2 UART0
    3,              # 3 GICD_CTLR / GICC_CTLR: group 0 and group 1
    1,              # 4
    0x01000000,     # 5 ITARGETSR byte 3: SPI 107 (= INTID 139) targets CPU0
    0x00000800,     # 6 ISENABLER4 bit 11: INTID 139
    0xff,           # 7 GICC_PMR
]
# A literal load is PC+imm, so the immediate depends on how many instructions
# follow it: assemble the list first, then encode.
def build(spin_only):
    body = [
        ('ldr', 0, 0), ('ldr', 1, 3), ('str', 1, 0, 0x000),
        ('ldr', 1, 5), ('str', 1, 0, 0x888),
        ('ldr', 1, 6), ('str', 1, 0, 0x110),
        ('ldr', 2, 1), ('ldr', 1, 3), ('str', 1, 2, 0x000),
        ('ldr', 1, 7), ('str', 1, 2, 0x004),
        ('mrs',), ('orr',), ('msr',),
    ]
    if not spin_only:
        body += [('ldr', 3, 2), ('ldr', 1, 4), ('str', 1, 3, 0x014)]
    body += [('wfi',), ('b',)]
    n, words = len(body), []
    for i, ins in enumerate(body):
        if ins[0] == 'ldr':
            words.append(0xe59f0000 | (ins[1] << 12) |
                         (n * 4 + 4 * ins[2] - (i * 4 + 8)))
        elif ins[0] == 'str':
            words.append(0xe5800000 | (ins[2] << 16) | (ins[1] << 12) | ins[3])
        elif ins[0] == 'mrs':
            words.append(0xe10f0000)
        elif ins[0] == 'orr':
            words.append(0xe3800080)
        elif ins[0] == 'msr':
            words.append(0xe121f000)
        elif ins[0] == 'wfi':
            words.append(0xe320f003)
        else:
            words.append(0xeafffffe)
    return words + CONST

elf(build(False), sys.argv[1] + '/parked.elf')
elf(build(True),  sys.argv[1] + '/masked.elf')
PY

    # 1. The parked guest ends the machine...
    timeout 30 "$QEMU" -M "ipq4019,nor=on,nor-file=$dir/nor.bin" \
        -kernel "$dir/parked.elf" -display none -monitor none -serial null \
        >"$dir/parked.log" 2>&1
    if [ $? -eq 0 ] && grep -q "ipq4019: poweroff: the guest parked the CPU" "$dir/parked.log"; then
        pass "a parked guest powers the board off (rc=0)"
    else
        fail "the parked guest did not power the board off: $(tail -1 "$dir/parked.log")"
    fi

    # 2. ...unless the board is told to stay on.
    timeout 4 "$QEMU" -M "ipq4019,nor=on,nor-file=$dir/nor2.bin,poweroff=off" \
        -kernel "$dir/parked.elf" -display none -monitor none -serial null \
        >"$dir/off.log" 2>&1
    [ $? -eq 124 ] && ! grep -q "ipq4019: poweroff:" "$dir/off.log" \
        && pass "poweroff=off keeps the machine running" \
        || fail "poweroff=off did not keep the machine running"

    # 3. Masking IRQs alone is not a power-off: nothing is pending, WFI halts.
    timeout 4 "$QEMU" -M "ipq4019,nor=on,nor-file=$dir/nor3.bin" \
        -kernel "$dir/masked.elf" -display none -monitor none -serial null \
        >"$dir/masked.log" 2>&1
    [ $? -eq 124 ] && ! grep -q "ipq4019: poweroff:" "$dir/masked.log" \
        && pass "an IRQ-masked WFI with nothing pending is not a power-off" \
        || fail "a plain masked WFI was taken for a power-off"
}

case_stages
case_cold
if [ "$QUICK" = 0 ]; then
    case_reset
    case_nand_kernel
    case_explicit_board
    case_cs_pin
    case_leds
    case_keys
    case_mdio
    case_ess
    case_preset
    case_qpic
    case_poweroff
    case_dts2xml
    case_vnc
    case_drive
    case_drive_ro
    case_boot_source
    case_refuse
    case_openwrt
    [ "$LIBVIRT" = 1 ] && case_libvirt
fi

say ""
say "$PASSES passed, $FAILS failed"
[ "$FAILS" = 0 ]
