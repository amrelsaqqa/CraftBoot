#!/bin/bash
# bypass.sh -- keep the QSEE bypass armed for as long as the machine runs.
#
# The image's SBL1 hands over to QSEE, which needs the QCOM protection fabric
# this machine does not model, so the harness redirects that handover: it stops
# the CPU where SBL1 is about to jump into QSEE and sets PC to the APPSBL entry
# with r0 = 0 ("monitor returned OK") in ARM state.
#
# Doing that with a one-shot gdb session only covers the first boot: a guest
# `reboot` (the qcom,pshold write) or a monitor system_reset starts the machine
# again at SBL1, and nothing would redirect the second handover -- the guest
# would simply execute QSEE and stall.  So keep gdb attached as a small agent:
# a breakpoint whose commands perform the redirect and continue, hit as often
# as the machine boots.
#
# The breakpoint is a *hardware* one on purpose.  A software breakpoint is a
# breakpoint instruction: SBL1 reloads its own image on every boot, so the breakpoint
# byte would disappear and the agent would never be told.  A hardware breakpoint
# matches on the address, so it survives the reload and any reuse of that DRAM.
#
#   source ./bypass.sh
#   bypass_start <gdb-socket> <machine-host-log> <agent-log> "<bp>:<entry>"
#   ...
#   bypass_stop
#
# The spec may also be the single word "auto": the agent then arms both known
# handover points (0xa8d1918 for the MikroTik/kt708 SBL1 family, 0x87e80000 for
# the Caraboot builds) and picks the APPSBL entry from the machine -- 0x87300000
# when something was placed there (Caraboot U-Boot), 0x80000000 when it is still
# zeroed.  The harnesses use that by default; a new SBL1 still needs an explicit
# spec.
#
# bypass_start waits for the machine to finish init before connecting: QEMU
# creates the gdb socket during chardev setup, long before the machine's memory
# exists, and a gdb that connects that early installs nothing and lets QSEE run
# for real (SBL1 then reports "Boot error ocuured!. Error code: 3027").
#
# A reset needs a fresh agent.  The hardware breakpoint stays in the vCPU
# across a reset, but the code around the handover is already translated by
# then, and QEMU only evaluates breakpoints when execution enters a translated
# block -- so the second handover went through unredirected and real QSEE ran,
# after which the boot went silent (QSEE clocks the UART its own way).  The
# sequence that works is: pause, reset, arm again (a new agent makes QEMU halt
# the guest, insert the breakpoint into freshly translated code, and resume):
#
#   bypass_reset <monitor-socket> <agent-log> "<bp>:<entry>"

BYPASS_GDB_PID=
BYPASS_GDB_SOCK=
BYPASS_GDB_LOG=
BYPASS_GDB_SPEC=

# Launch the agent and return as soon as the breakpoint is installed.  A fresh
# gdb connection makes QEMU halt the guest while the breakpoint is inserted, so
# the agent's trailing `continue` resumes it: the guest keeps running whatever
# the caller does next.
bypass_arm() {
    local sock=$1 glog=$2 spec=$3
    local bp entry cmds i

    : > "$glog"
    cmds="${glog%.log}.gcmds"
    if [ "$spec" = auto ]; then
        # Two entry points, one session.  0xa8d1918 is where the MikroTik SBL1
        # family calls into QSEE (its APPSBL/RouterBOOT is at 0x80000000);
        # the newer builds go straight to QSEE at 0x87e80000.  Their APPSBL is
        # U-Boot at 0x87300000 when the image ships one (the Caraboot dumps)
        # and the kernel/loader at 0x80000000 when it does not (kt708), which
        # the machine can answer directly: 0x87300000 is still zeroed if
        # nothing was placed there.
        cat > "$cmds" <<EOF
set pagination off
set confirm off
target remote $sock
hbreak *0xa8d1918
hbreak *0x87e80000
commands 1
silent
printf "bypass: trampoline pc=%p lr=%p -> 0x80000000\\n", \$pc, \$lr
set \$pc = 0x80000000
set \$r0 = 0
set \$cpsr = \$cpsr & ~0x20
continue
end
commands 2
silent
printf "bypass: qsee pc=%p lr=%p [0x87300000]=%p\\n", \$pc, \$lr, *(unsigned int *)0x87300000
if *(unsigned int *)0x87300000 != 0
  set \$pc = 0x87300000
else
  set \$pc = 0x80000000
end
set \$r0 = 0
set \$cpsr = \$cpsr & ~0x20
continue
end
continue
EOF
        gdb -batch -x "$cmds" > "$glog" 2>&1 &
        BYPASS_GDB_PID=$!
        BYPASS_GDB_SOCK=$sock
        BYPASS_GDB_LOG=$glog
        BYPASS_GDB_SPEC=$spec

        for i in $(seq 600); do
            grep -qa "Hardware assisted breakpoint 2 at" "$glog" && return 0
            grep -qa "^bypass: " "$glog" && return 0
            kill -0 "$BYPASS_GDB_PID" 2>/dev/null || break
            sleep 0.1
        done
        echo "bypass.sh: could not arm the auto breakpoints; see $glog" >&2
        return 1
    fi

    bp=${spec%%:*}
    entry=${spec##*:}
    cat > "$cmds" <<EOF
set pagination off
set confirm off
target remote $sock
hbreak *$bp
commands 1
silent
printf "bypass: pc=%p lr=%p r0=%p cpsr=%p\\n", \$pc, \$lr, \$r0, \$cpsr
set \$pc = $entry
set \$r0 = 0
set \$cpsr = \$cpsr & ~0x20
continue
end
continue
EOF

    gdb -batch -x "$cmds" > "$glog" 2>&1 &
    BYPASS_GDB_PID=$!
    BYPASS_GDB_SOCK=$sock
    BYPASS_GDB_LOG=$glog
    BYPASS_GDB_SPEC=$spec

    for i in $(seq 600); do
        grep -qa "Hardware assisted breakpoint 1 at $bp" "$glog" && return 0
        grep -qa "^bypass: " "$glog" && return 0
        kill -0 "$BYPASS_GDB_PID" 2>/dev/null || break
        sleep 0.1
    done
    echo "bypass.sh: could not arm the breakpoint at $bp; see $glog" >&2
    return 1
}

bypass_start() {
    local sock=$1 hostlog=$2 glog=$3 spec=$4
    local bp=${spec%%:*} i

    for i in $(seq 400); do
        grep -qa "ipq4019: boot-info handed" "$hostlog" 2>/dev/null && break
        sleep 0.05
    done

    bypass_arm "$sock" "$glog" "$spec" || return 1

    # the first boot must actually be redirected before the caller continues
    for i in $(seq 600); do
        grep -qa "^bypass: " "$glog" && return 0
        kill -0 "$BYPASS_GDB_PID" 2>/dev/null || break
        sleep 0.1
    done
    echo "bypass.sh: the redirect at $bp never ran; see $glog" >&2
    return 1
}

# Pause the machine, reset it, and arm a fresh agent.  Pausing first is what
# makes this race-free: SBL1 reaches the handover about 100 ms after a reset,
# which is far too fast to reconnect to.  Usable from another process (a test
# driver that owns the monitor), so the socket and log can be given explicitly:
#
#   bypass_reset <monitor-socket> [<gdb-socket> [<agent-log> [<bp>:<entry>]]]
bypass_reset() {
    local mon=$1
    local sock=${2:-$BYPASS_GDB_SOCK}
    local glog=${3:-$BYPASS_GDB_LOG}
    local spec=${4:-$BYPASS_GDB_SPEC}

    if [ -z "$sock" ] || [ -z "$spec" ]; then
        echo "bypass_reset: need a gdb socket and a bp:entry spec" >&2
        return 1
    fi

    [ -n "$mon" ] && python3 - "$mon" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(b"stop\n")
s.settimeout(2.0)
try:
    while s.recv(4096):
        pass
except OSError:
    pass
EOF

    bypass_stop
    bypass_arm "$sock" "$glog" "$spec"
}

bypass_stop() {
    if [ -n "$BYPASS_GDB_PID" ]; then
        kill -9 "$BYPASS_GDB_PID" 2>/dev/null
        wait "$BYPASS_GDB_PID" 2>/dev/null
        BYPASS_GDB_PID=
    fi
}
