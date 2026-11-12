#!/bin/sh
# Autonomous netinstall run: fresh guest + fresh installer, end to end.
#
#   sudo tools/ni-auto.sh                 one run
#   tools/ni-auto.sh -r 5                 five runs, stop early on success
#   tools/ni-auto.sh -r 5 -c              keep going after a success
#   tools/ni-auto.sh -k                   leave the VM running after a failure
#
# Artifacts per run: run/NN/{serial.log,qemu.err,netinstall.log,flash.bin}
#
# Order matters: netinstall-cli starts first (it blocks in "Wait for Link-UP"
# until the guest's tap has carrier), then the guest boots and its bootp
# attempt always finds a server.  Ctrl+E + Enter is still sent at the setup
# window so the etherboot path is forced; harmless if it already booted.
#
# Needs root for netinstall-cli (raw AF_PACKET).  Run the script under sudo, or
# allow exactly the binary + pkill:
#   amrelsaqqa ALL=(root) NOPASSWD: /path/to/netinstall-cli
#   amrelsaqqa ALL=(root) NOPASSWD: /usr/sbin/pkill -f netinstall-cli
set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
CLI=${CLI:-$HOME/Downloads/netinstall-7.15/netinstall-cli}
PACKAGE=${PACKAGE:?set PACKAGE to the installer package to serve}
IMG=${IMG:-$REPO/rb750g3-dump-7.17.bin}
BASE=${OUT:-$REPO/run}
PORT=${PORT:-4444}
IFACE=${IFACE:-ipqtap0}
PKILL=${PKILL:-/usr/sbin/pkill}
RUNS=1
TIMEOUT=240
EARLY_STOP=1
KEEP=

while [ $# -gt 0 ]; do
    case $1 in
    -r) RUNS=$2; shift 2 ;;
    -t) TIMEOUT=$2; shift 2 ;;
    -o) BASE=$2; shift 2 ;;
    -i) IMG=$2; shift 2 ;;
    -n) NPK=$2; shift 2 ;;
    -c) EARLY_STOP=; shift ;;
    -k) KEEP=1; shift ;;
    -h|--help) sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "ni-auto.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

SUDO=
if [ "$(id -u)" != 0 ]; then
    # note: `--help` exits 1 on this binary, so probe with sudo -l instead
    if sudo -n -l "$CLI" >/dev/null 2>&1; then
        SUDO="sudo -n"
    else
        echo "ni-auto.sh: need root for netinstall-cli. Add:" >&2
        echo "  echo \"$(id -un) ALL=(root) NOPASSWD: $CLI\" | sudo tee /etc/sudoers.d/zz-netinstall" >&2
        echo "  echo \"$(id -un) ALL=(root) NOPASSWD: $PKILL -f netinstall-cli\" | sudo tee -a /etc/sudoers.d/zz-netinstall" >&2
        exit 1
    fi
fi

[ -x "$CLI" ] || { echo "ni-auto.sh: no netinstall-cli at $CLI" >&2; exit 1; }
[ -r "$PACKAGE" ] || { echo "ni-auto.sh: no package at $PACKAGE" >&2; exit 1; }
ip link show "$IFACE" >/dev/null 2>&1 || { echo "ni-auto.sh: no tap $IFACE" >&2; exit 1; }

# --- helpers ---------------------------------------------------------------

# Stop a previous run.  pkill is called with exactly the sudoers args, so the
# caller's own command line must not contain the pattern (it never does here).
cleanup_previous() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    for f in "$BASE"/*/qemu.pid; do
        [ -f "$f" ] || continue
        kill "$(cat "$f")" 2>/dev/null || true
        rm -f "$f"
    done
    $SUDO "$PKILL" -f netinstall-cli >/dev/null 2>&1 || true
    free_port
    sleep 0.5
}

# Wait for the telnet port to be free; kill whatever still holds it (our qemu
# from a killed shell, typically).
free_port() {
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        ss -ltn 2>/dev/null | grep -q ":$PORT " || return 0
        pid=$(ss -ltnp 2>/dev/null | awk -v p=":$PORT" '$4 ~ p {print $6}' \
              | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | head -1)
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
        sleep 0.3
    done
    return 0
}

# wait_for <file> <pattern> <seconds>
wait_for() {
    n=0
    while [ $n -lt $(( $3 * 4 )) ]; do
        grep -q -- "$2" "$1" 2>/dev/null && return 0
        sleep 0.25
        n=$((n + 1))
    done
    return 1
}

# type_at <bytes-literal> <delay> — push bytes into the guest serial.
# Keeps the socket open for a moment after the write: QEMU's telnet chardev
# reads on its main loop, and closing immediately can drop the data.
type_at() {
    python3 - "$PORT" "$1" "${2:-0}" <<'PY' 2>/dev/null || true
import socket, sys, time
port, payload, delay = int(sys.argv[1]), sys.argv[2], float(sys.argv[3])
raw = payload.encode('latin1').decode('unicode_escape').encode('latin1')
try:
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    time.sleep(delay)
    s.sendall(raw)
    time.sleep(0.4)
    s.sendall(raw)          # second shot: first byte after connect is often eaten
    time.sleep(0.6)
    s.close()
except OSError:
    pass
PY
}

# monitor_cmd <hmp command> — talk to the HMP socket of the current run
monitor_cmd() {
    python3 - "$1" "$2" <<'PY' 2>/dev/null || true
import socket, sys
sock, cmd = sys.argv[1], sys.argv[2]
try:
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(10)
    s.connect(sock)
    s.sendall((cmd + "\n").encode())
    s.recv(4096)
    s.close()
except OSError:
    pass
PY
}

# Force the etherboot path.  The 2 s window opens right after
# "Press any key within 2 seconds to enter setup", so wait for *that* line
# (not the Ctrl+E banner, which is one line earlier), then send Ctrl+E, then
# Enter for the etherboot menu.
enter_etherboot() {
    wait_for "$1" "enter setup" 30 || return 0
    sleep 0.3
    type_at '\x05' 0
    type_at '\r' 1.0
    # If the booter ignored us it falls back to "kernel not found"; reset and
    # try once more while the installer is still serving.
    sleep 2
    if grep -q "kernel not found" "$1" 2>/dev/null; then
        monitor_cmd "$2" "system_reset"
        wait_for "$1" "enter setup" 20 || return 0
        sleep 0.3
        type_at '\x05' 0
        type_at '\r' 1.0
    fi
}

# verdict <serial.log>; sets RC
verdict() {
    if grep -q "could not mount filesystem" "$1" 2>/dev/null; then
        echo "   RESULT: FAIL - format ok, mount failed"; RC=1; return
    fi
    if grep -q "installation completed\|installed successfully" "$1" 2>/dev/null; then
        echo "   RESULT: SUCCESS"; RC=0; return
    fi
    if grep -q "Waiting for installation server" "$1" 2>/dev/null; then
        echo "   RESULT: FAIL - guest never heard from the installer"; RC=1; return
    fi
    if grep -q "Press ENTER to reboot" "$1" 2>/dev/null; then
        echo "   RESULT: FAIL - UI aborted"; RC=1; return
    fi
    echo "   RESULT: TIMEOUT - no terminal state"; RC=1
}

# --- runs ------------------------------------------------------------------

mkdir -p "$BASE"
RUN=1
RC=1
QPID=
while [ "$RUN" -le "$RUNS" ]; do
    CUR=$BASE/$(printf '%02d' "$RUN")
    rm -rf "$CUR"; mkdir -p "$CUR"
    echo "=== run $RUN/$RUNS -> $CUR"

    cleanup_previous

    $SUDO "$CLI" -i "$IFACE" "$PACKAGE" > "$CUR/netinstall.log" 2>&1 &
    CLIPID=$!
    sleep 1

    OUT="$CUR" IMG="$IMG" "$REPO/tools/ni-vm.sh" -t "$PORT" ${VMFLAGS:-} >/dev/null
    QPID=$(cat "$CUR/qemu.pid")

    if wait_for "$CUR/serial.log" "enter setup" 30; then
        enter_etherboot "$CUR/serial.log" "$CUR/monitor.sock"
    fi
    wait_for "$CUR/serial.log" "jumping to kernel code" 60 || \
        echo "   warn: netboot kernel not reached"

    END=0
    n=0
    while [ $n -lt $((TIMEOUT * 4)) ]; do
        if grep -q "Press ENTER to reboot\|could not mount filesystem\|installation completed" \
                "$CUR/serial.log" 2>/dev/null; then END=1; break; fi
        kill -0 "$CLIPID" 2>/dev/null || { END=1; break; }
        sleep 0.25
        n=$((n + 1))
    done

    sleep 2.5                              # let FLASH_SAVE write the final image
    echo "--- guest console tail"
    tail -10 "$CUR/serial.log" 2>/dev/null | sed 's/^/   /'
    verdict "$CUR/serial.log"
    [ $END -eq 0 ] && echo "   (timeout after ${TIMEOUT}s)"

    if [ -n "$KEEP" ] && [ $RC -ne 0 ]; then
        $SUDO "$PKILL" -f netinstall-cli >/dev/null 2>&1 || true
        echo "--- artifacts: $CUR"
        echo "=== leaving VM running (pid $QPID) with flash $CUR/flash.bin"
        exit $RC
    fi
    kill "$QPID" 2>/dev/null || true
    $SUDO "$PKILL" -f netinstall-cli >/dev/null 2>&1 || true
    wait "$CLIPID" 2>/dev/null || true
    QPID=
    echo "--- artifacts: $CUR"
    [ -s "$CUR/flash.bin" ] || echo "   warn: no flash dump"

    if [ $RC -eq 0 ] && [ -n "$EARLY_STOP" ]; then
        echo "=== stopping after first success"
        exit 0
    fi
    RUN=$((RUN + 1))
done
exit $RC
