#!/bin/bash
# entrypoint.sh -- launcher for the packaged image.
#
# Everything image-specific the machine needs is an environment variable, so a
# user never edits a script:
#
#   DUMP=/dumps/foo.bin        the flash dump (default: first *.bin in /dumps)
#   SERIAL=stdio|tcp|pty       where UART0 goes (default stdio)
#   SERIAL_PORT=4444           TCP port for SERIAL=tcp
#   NET=lan|none|bridge|user   what the switch's lan1 is plugged into
#                              lan (default): a tap in this container
#                              bridge: that tap joined to IPQ_BRIDGE
#                              user: slirp -- no tap, no NET_ADMIN, and the
#                                    only mode that works on Docker Desktop
#   LAN_IP=192.168.1.2/24      NET=lan: fixed address for the tap (DHCP=0)
#   DHCP=1                     NET=lan: ask the guest for a lease instead
#   IPQ_BRIDGE=ipqbr0          NET=bridge: bridge to join
#   IPQ_PORT=5                 switch port the netdev is wired to (5 = lan1)
#   LOG_DIR=/var/log/ipq4019   transcripts and the QEMU host log
#
# NET=user knobs (slirp, defaults follow the dump):
#   LAN_NET=192.168.88.0/24    slirp's network; must contain the guest
#   LAN_HOST=192.168.88.2      the address slirp gives the host end
#   GUEST_IP=192.168.88.1      the guest's address on that network
#   HOSTFWD="tcp:127.0.0.1:8080-80;tcp:127.0.0.1:8443-443"
#                              host:port -> GUEST_IP:port (publish them with
#                              docker run -p; slirp does not route from a
#                              non-Linux host on its own)
#
# A tap needs NET_ADMIN and /dev/net/tun, so the container has to be started
# with those; without them the launcher still boots the machine and says why
# the network is off.  On Docker Desktop (Windows/macOS) a tap lives inside the
# Linux VM and cannot be reached from the host -- use NET=user there:
#
#   docker run --rm -it --cap-add NET_ADMIN --device /dev/net/tun \
#              -v ~/dumps:/dumps:ro ipq4019
#   docker run --rm -it -p 8080:8080 -e NET=user \
#              -e HOSTFWD='tcp:0.0.0.0:8080-80' -v ~/dumps:/dumps:ro ipq4019
#
# The QSEE handover is skipped by the machine itself: it reads the stage table
# out of the image and resumes at APPSBL, on every boot including a `reboot`
# from the guest.  No gdb agent is involved (that was needed before the skip
# moved into the machine, and installing it now races with the boot).
set -u
cd "$(dirname "$0")" || exit 1

QEMU=${QEMU_BIN:-/usr/local/bin/qemu-system-arm}
LOG_DIR=${LOG_DIR:-/var/log/ipq4019}
SERIAL=${SERIAL:-stdio}
SERIAL_PORT=${SERIAL_PORT:-4444}
NET=${NET:-lan}
IPQ_PORT=${IPQ_PORT:-5}
DHCP=${DHCP:-1}
TAP=${TAP:-tap0}

say() { printf '%s\n' "$*" >&2; }
die() { say "ipq4019: $*"; exit 2; }

mkdir -p "$LOG_DIR" 2>/dev/null || LOG_DIR=/tmp
HOSTLOG=$LOG_DIR/host.log
CONLOG=$LOG_DIR/console.log
rm -f "$HOSTLOG" "$LOG_DIR/dhcp.log"

# ---------------------------------------------------------------- the dump
if [ -z "${DUMP:-}" ]; then
    for f in /dumps/*.bin; do
        [ -e "$f" ] && { DUMP=$f; break; }
    done
fi
[ -n "${DUMP:-}" ] || die "no dump: mount one at /dumps or set DUMP=/path/to/dump.bin"
[ -r "$DUMP" ] || die "cannot read $DUMP"

# Which board this dump belongs to decides the LAN defaults only; the boot
# chain and its QSEE handover are read from the image by the machine.
IS_MIKROTIK=0
grep -qa "RouterBOARD" "$DUMP" && IS_MIKROTIK=1
if [ -z "${LAN_IP:-}" ]; then
    if [ "$IS_MIKROTIK" = 1 ]; then LAN_IP=192.168.88.2/24
    else                            LAN_IP=192.168.1.2/24; fi
fi

# ---------------------------------------------------------------- the UART
case $SERIAL in
    stdio) SERARGS=(-chardev "stdio,id=con,logfile=$CONLOG,logappend=on"
                    -serial chardev:con) ;;
    tcp)   SERARGS=(-chardev "socket,id=con,host=0.0.0.0,port=$SERIAL_PORT,\
server=on,wait=off,logfile=$CONLOG,logappend=on" -serial chardev:con) ;;
    pty)   SERARGS=(-chardev "pty,id=con,path=$LOG_DIR/console.tty,\
logfile=$CONLOG,logappend=on" -serial chardev:con) ;;
    *)     die "SERIAL=$SERIAL: use stdio, tcp or pty" ;;
esac

# ------------------------------------------------------------- the network
# The tap only exists once QEMU opened it, so it is addressed after launch.
NETARGS=()
TAP_POST=()
case $NET in
    none)
        ;;
    lan|bridge)
        NETARGS=(-netdev "tap,id=net0,ifname=$TAP,script=no,downscript=no")
        export QEMU_IPQ_NETDEV=net0
        export QEMU_IPQ_ETH_BRIDGE_PORT=$IPQ_PORT
        if [ "$NET" = bridge ]; then
            [ -n "${IPQ_BRIDGE:-}" ] || \
                die "NET=bridge needs IPQ_BRIDGE=<existing bridge name>"
            TAP_POST+=("ip link set $TAP master $IPQ_BRIDGE")
        fi
        TAP_POST+=("ip link set $TAP up")
        ;;
    user|slirp)
        # slirp terminates the link itself: nothing to configure on the host,
        # so this mode works wherever Docker runs, including Docker Desktop
        # on Windows, where a tap would live inside the Linux VM.
        if [ "$IS_MIKROTIK" = 1 ]; then
            LAN_NET=${LAN_NET:-192.168.88.0/24}
            LAN_HOST=${LAN_HOST:-192.168.88.2}
            GUEST_IP=${GUEST_IP:-192.168.88.1}
        else
            LAN_NET=${LAN_NET:-192.168.1.0/24}
            LAN_HOST=${LAN_HOST:-192.168.1.2}
            GUEST_IP=${GUEST_IP:-192.168.1.1}
        fi
        NETDEV="user,id=net0,net=$LAN_NET,host=$LAN_HOST"
        if [ -n "${HOSTFWD:-}" ]; then
            IFS=';' read -r -a fwds <<< "$HOSTFWD"
            for fwd in "${fwds[@]}"; do
                case $fwd in
                    *-*:*) ;;                       # -guestip:port already
                    *-*)   fwd="${fwd%-*}-$GUEST_IP:${fwd##*-}" ;;
                esac
                NETDEV="$NETDEV,hostfwd=$fwd"
            done
        fi
        NETARGS=(-netdev "$NETDEV")
        export QEMU_IPQ_NETDEV=net0
        export QEMU_IPQ_ETH_BRIDGE_PORT=$IPQ_PORT
        ;;
    *)  die "NET=$NET: use lan, bridge, user or none" ;;
esac

# ------------------------------------------------------------------ launch
say "ipq4019: dump   $DUMP"
say "ipq4019: serial $SERIAL${SERIAL_PORT:+ (port $SERIAL_PORT)} -> $CONLOG"
if [ "$NET" = none ]; then
    say "ipq4019: net    off"
elif [ "$NET" = user ] || [ "$NET" = slirp ]; then
    say "ipq4019: net    slirp $LAN_NET, guest $GUEST_IP"
    [ -n "${HOSTFWD:-}" ] && say "ipq4019: fwd    $HOSTFWD (publish with -p)"
else
    say "ipq4019: net    $NET on $TAP, switch port $IPQ_PORT"
fi

QEMU_IPQ_BOOTDEV_TYPE=${QEMU_IPQ_BOOTDEV_TYPE:-6} \
"$QEMU" -M ipq4019 -kernel "$DUMP" -display none -monitor none \
    "${SERARGS[@]}" "${NETARGS[@]}" \
    ${QEMU_IPQ_EXTRA:-} > "$HOSTLOG" 2>&1 &
QP=$!

cleanup() {
    kill -9 $QP 2>/dev/null
    wait $QP 2>/dev/null
    exit 0
}
trap cleanup EXIT INT TERM

sleep 0.3
kill -0 $QP 2>/dev/null || { sed 's/^/  /' "$HOSTLOG" >&2; die "QEMU did not stay up"; }

# ------------------------------------------------------------ the host side
if [ "${#TAP_POST[@]}" -gt 0 ]; then
    ok=0
    for _ in $(seq 100); do
        ip link show "$TAP" >/dev/null 2>&1 && { ok=1; break; }
        sleep 0.05
    done
    if [ "$ok" = 0 ]; then
        say "ipq4019: no $TAP: the container needs --cap-add NET_ADMIN"
        say "         and --device /dev/net/tun; the machine still runs."
    else
        for c in "${TAP_POST[@]}"; do
            $c 2>/dev/null || say "ipq4019: warning: '$c' failed"
        done
        if [ "$DHCP" = 1 ] && [ "$NET" = lan ]; then
            # The guest is the DHCP server, but its userspace only exists once
            # the kernel booted (~35 s), so keep asking.
            (
                for _ in $(seq 40); do
                    ip -4 addr show dev "$TAP" 2>/dev/null | grep -q "inet " && exit 0
                    if lease=$(python3 "$PWD/dhcp_client.py" "$TAP" 8 \
                                     2>> "$LOG_DIR/dhcp.log"); then
                        eval "$lease"
                        ip addr add "$ip/$prefix" dev "$TAP" 2>/dev/null && exit 0
                    fi
                    sleep 1
                done
            ) &
        elif [ "$NET" = lan ]; then
            ip addr add "$LAN_IP" dev "$TAP" 2>/dev/null || \
                say "ipq4019: warning: cannot add $LAN_IP to $TAP"
        fi
        ip -br addr show "$TAP" 2>/dev/null | sed 's/^/ipq4019: /' >&2
    fi
fi

if [ "$SERIAL" = pty ]; then
    for _ in $(seq 50); do [ -e "$LOG_DIR/console.tty" ] && break; sleep 0.1; done
    say "ipq4019: attach with: picocom -b 115200 $LOG_DIR/console.tty"
fi
[ "$SERIAL" = tcp ] && say "ipq4019: UART on port $SERIAL_PORT"

wait $QP
