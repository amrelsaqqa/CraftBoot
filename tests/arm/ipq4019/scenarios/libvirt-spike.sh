#!/bin/bash
# tests/libvirt-spike.sh -- the machine as a libvirt domain, end to end.
#
# The point of the SoC/board split was to make this machine describable from
# outside: a board tree becomes a domain XML, the dies become block backends,
# and libvirt owns the run.  This script does exactly that and checks that the
# boot chain comes up, so "libvirt-compatible" is a fact and not a claim.
#
#   ./tests/libvirt-spike.sh                    # MikroTik dump, its own DTB
#   ./tests/libvirt-spike.sh --dump=/path.bin
#   ./tests/libvirt-spike.sh --dtb=/path.dtb --die=/path/die.img
#
# It runs a *private* virtqemud (its own XDG dirs and socket) so the user's
# session libvirt is never touched, and it needs one local concession that is
# explained below: SELinux will not let libvirt transition to svirt_tcg_t for a
# binary built in a home directory, so the private driver confines nothing.
#
# Everything the domain needs is generated: the board properties come from the
# device tree, the die is an empty file that the machine programs on first
# boot, and the console goes to a file this script can read.
set -u
cd "$(dirname "$0")/.." || exit 1

DUMP=$HOME/mikrotik-dump-latest7.17-32m.bin
DTB=
DIE=
IMAGE=$HOME/mikrotik-dump-latest7.17-32m-rb-patched-1M.bin
KEEP=0
for a in "$@"; do
    case $a in
        --dump=*)  DUMP=${a#*=} ;;
        --dtb=*)   DTB=${a#*=} ;;
        --die=*)   DIE=${a#*=} ;;
        --image=*) IMAGE=${a#*=} ;;
        --keep)    KEEP=1 ;;
        -h|--help) sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "libvirt-spike: unknown argument $a" >&2; exit 2 ;;
    esac
done

command -v virsh >/dev/null || { echo "libvirt-spike: no virsh" >&2; exit 1; }
command -v virtqemud >/dev/null || { echo "libvirt-spike: no virtqemud" >&2; exit 1; }
[ -x ./build/qemu-system-arm ] || { echo "libvirt-spike: no build/qemu-system-arm" >&2; exit 1; }
[ -r "$IMAGE" ] || { echo "libvirt-spike: no $IMAGE (use --image=)" >&2; exit 1; }

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/libvirt-spike.XXXXXX") || exit 1
RUN=$SCRATCH/run
SOCK=$RUN/libvirt/virtqemud-sock
URI="qemu+unix:///session?socket=$SOCK"
CONSOLE=$SCRATCH/console.log
PASSES=0 FAILS=0
DAEMON=
pass() { PASSES=$((PASSES + 1)); echo "  ok   $*"; }
fail() { FAILS=$((FAILS + 1)); echo "  FAIL $*"; }

cleanup() {
    if [ -n "$DAEMON" ]; then
        virsh -c "$URI" destroy ipq4019-spike >/dev/null 2>&1
        virsh -c "$URI" undefine ipq4019-spike >/dev/null 2>&1
        kill "$DAEMON" 2>/dev/null
        wait "$DAEMON" 2>/dev/null
    fi
    [ "$KEEP" = 1 ] || rm -rf "$SCRATCH"
    [ "$KEEP" = 1 ] && echo "  kept $SCRATCH"
}
trap cleanup EXIT INT TERM

wait_file() {  # wait_file <path> <seconds>
    local i
    for ((i = 0; i < $2 * 10; i++)); do
        [ -e "$1" ] && return 0
        sleep 0.1
    done
    return 1
}

wait_text() {  # wait_text <file> <pattern> <seconds>
    local i
    for ((i = 0; i < $3 * 2; i++)); do
        grep -aq -- "$2" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}

# ---- a private daemon ----------------------------------------------------
#
# libvirt reads its session configuration from $XDG_CONFIG_HOME/libvirt and its
# sockets from $XDG_RUNTIME_DIR, so pointing both at the scratch dir gives a
# daemon that shares nothing with the one the user may be running.
mkdir -p "$RUN/libvirt" "$SCRATCH/xdg/libvirt" "$SCRATCH/cache"
cat > "$SCRATCH/xdg/libvirt/qemu.conf" <<EOF
# Private configuration for tests/libvirt-spike.sh.
#
# security_driver = "none" is the one compromise this machine needs under
# SELinux: libvirt asks the kernel for a transition from the user's domain to
# svirt_tcg_t, and the QEMU policy only allows that for its own binary
# (qemu_exec_t under /usr).  A machine type built in a home directory is
# therefore refused with EACCES unless confinement is off.  The domain still
# runs as this user, which is all a session-domain run guarantees anyway.
security_driver = "none"
dynamic_ownership = 0
EOF
chmod 600 "$SCRATCH/xdg/libvirt/qemu.conf"

XDG_CONFIG_HOME=$SCRATCH/xdg XDG_RUNTIME_DIR=$RUN \
XDG_CACHE_HOME=$SCRATCH/cache \
    virtqemud -t 300 >"$SCRATCH/daemon.log" 2>&1 &
DAEMON=$!
if ! wait_file "$SOCK" 10; then
    echo "libvirt-spike: the private virtqemud did not come up:" >&2
    sed 's/^/    /' "$SCRATCH/daemon.log" >&2
    exit 1
fi
pass "a private virtqemud is up at $SOCK"

# ---- the board description ----------------------------------------------
if [ -z "$DTB" ]; then
    DTB=$SCRATCH/board.dtb
    python3 - "$DUMP" "$DTB" <<'PY' || exit 1
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
PY
    pass "the board's device tree came out of the dump"
fi

: > "$SCRATCH/nor.img"
[ -n "$DIE" ] && cp "$DIE" "$SCRATCH/nor.img"

./tools/dts2xml.py "$DTB" \
    --name ipq4019-spike \
    --emulator "$PWD/build/qemu-system-arm" \
    --image "$IMAGE" \
    --console "$CONSOLE" \
    --nor-size 32M \
    --nor-file "$SCRATCH/nor.img" \
    --key button=esc \
    --key mode-button=f \
    >"$SCRATCH/domain.xml" 2>"$SCRATCH/dts2xml.err"
[ $? -eq 0 ] || { echo "libvirt-spike: dts2xml failed"; sed 's/^/    /' "$SCRATCH/dts2xml.err"; exit 1; }
pass "the board tree became a libvirt domain"

# ---- run it --------------------------------------------------------------
if ! virsh -c "$URI" define "$SCRATCH/domain.xml" >"$SCRATCH/define.log" 2>&1; then
    fail "libvirt refused the domain"
    sed 's/^/    /' "$SCRATCH/define.log"
else
    pass "libvirt accepted the machine type and the domain"
fi

if virsh -c "$URI" start ipq4019-spike >"$SCRATCH/start.log" 2>&1; then
    pass "the domain started"
else
    fail "the domain did not start"
    sed 's/^/    /' "$SCRATCH/start.log"
fi

if wait_text "$CONSOLE" "RouterBOOT" 90; then
    pass "the boot chain reached RouterBOOT under libvirt"
else
    fail "RouterBOOT never appeared on the domain's console"
    tail -5 "$CONSOLE" 2>/dev/null | sed 's/^/    /'
fi

# The die is a libvirt-run block backend, so it exists as a 32 MiB part that the
# machine programmed from the image -- the same check the plain -drive case
# makes, through libvirt's process.
SIZE=$(stat -c %s "$SCRATCH/nor.img" 2>/dev/null || echo 0)
[ "$SIZE" -ge 33554432 ] \
    && pass "the NOR die is a 32 MiB backend the machine programmed" \
    || fail "the die is $SIZE bytes, not a programmed 32 MiB part"

# What the board became, from the machine's own log: libvirt's domain log is
# where QEMU's stderr goes.
LOG=$(ls "$SCRATCH"/cache/libvirt/qemu/log/ipq4019-spike.log 2>/dev/null | head -1)
if [ -n "$LOG" ]; then
    grep -aq "ipq4019: leds: user-led@3 power-led@0 usb-power-off@2(al)" "$LOG" \
        && pass "the lamps from the tree are wired to their pins" \
        || fail "the board's lamps did not reach the machine"
    grep -aq "ipq4019: mdio@00090000, 6 port(s): 0@1000 1@1000 2@1000 3@1000 4@1000 5@1000" "$LOG" \
        && pass "the tree's five PHYs plus the switch's PSGMII block are the bus" \
        || fail "the tree's PHYs did not reach the MDIO bus"
fi

echo
echo "$PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
