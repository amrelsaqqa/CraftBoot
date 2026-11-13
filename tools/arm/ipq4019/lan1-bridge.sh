#!/bin/bash
# lan1-bridge.sh -- the emulated router on a virtual (or real) wire, UART0 on a socket.
#
#   sudo ./lan1-bridge.sh                          # default dump, virtual cable
#   sudo ./lan1-bridge.sh ~/kt708-ipq-...bin       # OpenWrt image instead
#   sudo NIC=enp0s31f6 ./lan1-bridge.sh            # also put it on a real NIC
#   sudo IPQ_PORT=1 ./lan1-bridge.sh               # wire ether1 instead of lan1
#   sudo DHCP=0 ./lan1-bridge.sh                   # fixed host address, no DHCP
#   ./lan1-bridge.sh --print                       # just the QEMU command, no root
#
# Two modes, same idea: the machine's switch port is a tap, and the host is on
# the other end of that "cable".
#
#   virtual (default, NIC empty)      real NIC (NIC=enp0s31f6)
#
#      host IP ── tap0 ── QEMU       host IP ── ipqbr0 ── tap0 ── QEMU
#                                                     └── enp0s31f6 ── wire
#
# The virtual mode needs nothing but /dev/net/tun and NET_ADMIN, and it does
# not disturb any of your own networking.  It is still real Ethernet at layer 2:
# the host and the guest share a broadcast domain, so LuCI/ssh, DHCP, TFTP and
# netinstall all work exactly as they would over a cable.  Add NIC=... only
# when you want the guest on an actual wire.
#
# The UART is a TCP socket rather than a pty so it works from anywhere:
#
#     socat -,raw,echo=0 TCP:127.0.0.1:4444      # like picocom, over TCP
#     nc 127.0.0.1 4444                          # crude but fine for reading
#
# Everything it creates is removed again on exit (Ctrl-C), including the
# address dhclient put on the tap/bridge.
#
# Knobs:
#   NIC=""            physical port to bridge ("" = virtual cable only)
#   BR=ipqbr0         bridge name (only used when NIC is set)
#   TAP=tap0       tap name
#   IPQ_PORT=5        switch port: 0=cpu 1=ether1/wan 2=lan4 3=lan3 4=lan2 5=lan1
#   UART_PORT=4444    TCP port for UART0
#   UART_BIND=127.0.0.1   bind address (0.0.0.0 to reach it from another machine)
#   DHCP=1            lease an address from the guest's own DHCP server
#   STATIC_IP=        fixed host address (default: 192.168.1.2/24 OpenWrt,
#                     192.168.88.2/24 RouterOS; also the DHCP fallback)
#   FORCE=1           allow bridging a NIC that already has an address
#   LOG=/tmp/ipq-lan  transcripts and logs
#
# The board is a -machine property list, one list per run:
#
#   --board=NAME      preset to start from: default, none, hap-ac2, hap-ac3
#   --persist[=/path] the NOR die is that file, so programs and erases in it
#                     outlive Ctrl-C (default $LOG/flash.bin)
#   --nor-size=N      NOR die bytes (--flash-size= still works)
#   --nor-id=ID       NOR RDID (--flash-jedec= still works)
#   --nand            put a blank SPI NAND on chip select 1 (132 MiB)
#   --nand-persist[=/path]  ... and make its die a file too ($LOG/nand.bin)
#   --nand-size=N / --nand-id=ID
#   --qpic-nand       put a parallel NAND behind the QPIC (the hap-ac3 preset
#                     already has one; this adds it to any other board)
#   --qpic-persist[=/path]  ... and make that die a file ($LOG/qpic-nand.bin),
#                     which is what netinstall needs to have something to fill
#   --qpic-size=N / --qpic-id=ID
#   --boot=DEVICE     which die the boot chain is read from: spi-nor (default),
#                     spi-nand, qpic-nand, none
#
#   sudo ./lan1-bridge.sh --persist[=/path] [--nor-size=N] [--nor-id=ID]
#   sudo ./lan1-bridge.sh --persist /tmp/nor/708.bin --nor-size=8388608 \
#        --nor-id=ef4017 ~/kt708-ipq-openwrt24.10.0-no-wifi.bin
#   sudo ./lan1-bridge.sh --board=hap-ac2 ~/mikrotik-dump-latest7.17-32m.bin
#
# A value may be attached with `=` or given as the next argument; with none,
# --persist uses $LOG/flash.bin and --nand-persist $LOG/nand.bin.  The space
# form needs another argument after it (with the value last it could be the
# dump), so write `--persist=<path>` in that case.
#
# Nothing here is configured through the environment any more: the options
# become `-machine` properties, which is why they survive the sudo re-exec as
# argv.  The deprecated QEMU_IPQ_* die variables are still understood as the
# *request* (translated once, then removed so the machine's compatibility shim
# cannot override the properties), and an inline `sudo VAR=...` works too.
#
# --print writes the QEMU command line to stdout and exits before touching the
# network, which is the way to check what a run would actually be.
#
# The dump is a *programming* source: the machine boots the SBL1 ELF out of the
# die and only copies the image into a die that has no boot chain yet.  So the
# dump is passed as -kernel only when it can be used, and the banner's `boot`
# line says which of the two actually ran.
set -u

# ------------------------------------------------------------------ options
# Before the sudo re-exec, so they arrive as plain argv on the other side.
# The loop below consumes argv, so keep the original list: the re-exec has to
# hand over the options themselves, not what is left of them.  (A shell
# variable such as PERSIST would not survive either -- `sudo -E` passes the
# environment, and these were never exported.)
#
# --persist and --nand-persist take an optional path, and `=` is the way to be
# sure about it: with the value as the last word of the command line, it cannot
# be told apart from the dump that follows (`--persist flash.bin dump.bin` is a
# die next to a dump; `--persist dump.bin` would be either).  So the space form
# is only accepted while another argument remains, and the ambiguous form says
# which one to use instead of guessing.
ORIG=("$@")
ambig() {
    echo "lan1-bridge.sh: $1 <path> at the end of the command line is" \
         "ambiguous -- it could be the dump.  Write $1=<path>." >&2
    exit 1
}

BOARD=${QEMU_IPQ_BOARD:-}
PERSIST=${QEMU_IPQ_FLASH_PERSIST:-}
FLASH_SIZE=${QEMU_IPQ_FLASH_SIZE:-}
FLASH_JEDEC=${QEMU_IPQ_FLASH_JEDEC:-}
NAND=${QEMU_IPQ_NAND_PERSIST:-}
NAND_ON=${QEMU_IPQ_NAND:-}
NAND_SIZE=${QEMU_IPQ_NAND_SIZE:-}
NAND_ID=${QEMU_IPQ_NAND_ID:-}
QPIC=
QPIC_ON=
QPIC_SIZE=
QPIC_ID=
BOOT=
PRINT=0
POS=()
while [ $# -gt 0 ]; do
    case $1 in
        --board)          BOARD=${2:?--board needs a value}; shift 2 ;;
        --board=*)        BOARD=${1#*=}; shift ;;
        --persist)        if [ $# -ge 3 ] && [ "${2#-}" = "$2" ]; then
                              PERSIST=$2; shift 2
                          elif [ $# -eq 2 ] && [ "${2#-}" = "$2" ]; then
                              ambig --persist
                          else
                              PERSIST=default; shift
                          fi ;;
        --persist=*)      PERSIST=${1#*=}; shift ;;
        --nor-size|--flash-size)
                          FLASH_SIZE=${2:?$1 needs a value}; shift 2 ;;
        --nor-size=*|--flash-size=*)
                          FLASH_SIZE=${1#*=}; shift ;;
        --nor-id|--flash-jedec)
                          FLASH_JEDEC=${2:?$1 needs a value}; shift 2 ;;
        --nor-id=*|--flash-jedec=*)
                          FLASH_JEDEC=${1#*=}; shift ;;
        --nand)           NAND_ON=1; shift ;;
        --nand-persist)   NAND_ON=1
                          if [ $# -ge 3 ] && [ "${2#-}" = "$2" ]; then
                              NAND=$2; shift 2
                          elif [ $# -eq 2 ] && [ "${2#-}" = "$2" ]; then
                              ambig --nand-persist
                          else
                              NAND=default; shift
                          fi ;;
        --nand-persist=*) NAND_ON=1; NAND=${1#*=}; shift ;;
        --nand-size)      NAND_SIZE=${2:?--nand-size needs a value}; shift 2 ;;
        --nand-size=*)    NAND_SIZE=${1#*=}; shift ;;
        --nand-id)        NAND_ID=${2:?--nand-id needs a value}; shift 2 ;;
        --nand-id=*)      NAND_ID=${1#*=}; shift ;;
        --qpic-nand)      QPIC_ON=1; shift ;;
        --qpic-persist)   QPIC_ON=1
                          if [ $# -ge 3 ] && [ "${2#-}" = "$2" ]; then
                              QPIC=$2; shift 2
                          elif [ $# -eq 2 ] && [ "${2#-}" = "$2" ]; then
                              ambig --qpic-persist
                          else
                              QPIC=default; shift
                          fi ;;
        --qpic-persist=*) QPIC_ON=1; QPIC=${1#*=}; shift ;;
        --qpic-size)      QPIC_SIZE=${2:?--qpic-size needs a value}; shift 2 ;;
        --qpic-size=*)    QPIC_SIZE=${1#*=}; shift ;;
        --qpic-id)        QPIC_ID=${2:?--qpic-id needs a value}; shift 2 ;;
        --qpic-id=*)      QPIC_ID=${1#*=}; shift ;;
        --boot)           BOOT=${2:?--boot needs a value}; shift 2 ;;
        --boot=*)         BOOT=${1#*=}; shift ;;
        -n|--print)       PRINT=1; shift ;;
        -h|--help)        sed -n '2,/^set -u/p' "$0" |
                          sed '$d; s/^# \{0,1\}//' >&2; exit 0 ;;
        -*)               echo "lan1-bridge.sh: unknown option $1" >&2; exit 1 ;;
        *)                POS+=("$1"); shift ;;
    esac
done
set -- ${POS+"${POS[@]}"}

# The setup needs NET_ADMIN; everything after it is plain QEMU.  Hand over the
# argv that was given to us, so the options survive as options.  --print needs
# no privileges: it only describes the run.
if [ "$PRINT" != 1 ] && [ "$(id -u)" != 0 ]; then
    exec sudo -E "$0" ${ORIG+"${ORIG[@]}"}
fi

cd "$(dirname "$0")" || exit 1

NIC=${NIC-}
BR=${BR:-ipqbr0}
TAP=${TAP:-tap0}
IPQ_PORT=${IPQ_PORT:-5}
UART_PORT=${UART_PORT:-4444}
UART_BIND=${UART_BIND:-127.0.0.1}
DHCP=${DHCP:-1}
FORCE=${FORCE:-0}
LOG=${LOG:-/tmp/ipq-lan}
DUMP=${1:-$HOME/mikrotik-dump-latest7.17-32m.bin}

IS_MIKROTIK=0
grep -qa "RouterBOARD" "$DUMP" 2>/dev/null && IS_MIKROTIK=1
if [ -z "${STATIC_IP:-}" ]; then
    if [ "$IS_MIKROTIK" = 1 ]; then STATIC_IP=192.168.88.2/24
    else                            STATIC_IP=192.168.1.2/24; fi
fi

say() { printf '%s\n' "$*" >&2; }
die() { say "lan1-bridge: $*"; exit 1; }

[ -r "$DUMP" ] || die "cannot read $DUMP"
mkdir -p "$LOG" || die "cannot create $LOG"

# --persist without a value keeps its die next to the other logs.
[ "$PERSIST" = default ] && PERSIST=$LOG/flash.bin
[ "$NAND" = default ] && NAND=$LOG/nand.bin
if [ -n "$NAND" ]; then
    NAND_ON=1
fi
[ "$QPIC" = default ] && QPIC=$LOG/qpic-nand.bin
if [ -n "$QPIC" ]; then
    QPIC_ON=1
fi
# "NAND=0" in the environment is a request for no NAND, which must not read as
# a non-empty string further down.
[ "${NAND_ON:-}" = 0 ] && NAND_ON=

# The dies are -machine values, and -machine splits on commas: a path with one
# in it cannot be expressed, so refuse it instead of letting QEMU chop the
# property list in two.
for f in "$PERSIST" "$NAND" "$QPIC"; do
    case $f in
        *,*) die "'$f' contains a comma, which -machine cannot carry" ;;
    esac
done

# The board, as one property list: preset first, then the explicitly given
# parts, which always win over it.
MACHINE=ipq4019
[ -n "$BOARD" ]       && MACHINE="$MACHINE,board=$BOARD"
# Naming a die's file is also a statement that the board has that die.
[ -n "$PERSIST" ]     && MACHINE="$MACHINE,nor=on"
[ -n "$PERSIST" ]     && MACHINE="$MACHINE,nor-file=$PERSIST"
[ -n "$FLASH_SIZE" ]  && MACHINE="$MACHINE,nor-size=$FLASH_SIZE"
[ -n "$FLASH_JEDEC" ] && MACHINE="$MACHINE,nor-id=$FLASH_JEDEC"
[ -n "$NAND_ON" ]     && MACHINE="$MACHINE,nand=on"
[ -n "$NAND" ]        && MACHINE="$MACHINE,nand-file=$NAND"
[ -n "$NAND_SIZE" ]   && MACHINE="$MACHINE,nand-size=$NAND_SIZE"
[ -n "$NAND_ID" ]     && MACHINE="$MACHINE,nand-id=$NAND_ID"
[ -n "$QPIC_ON" ]     && MACHINE="$MACHINE,qpic-nand=on"
[ -n "$QPIC" ]        && MACHINE="$MACHINE,qpic-nand-file=$QPIC"
[ -n "$QPIC_SIZE" ]   && MACHINE="$MACHINE,qpic-nand-size=$QPIC_SIZE"
[ -n "$QPIC_ID" ]     && MACHINE="$MACHINE,qpic-nand-id=$QPIC_ID"
[ -n "$BOOT" ]        && MACHINE="$MACHINE,boot-device=$BOOT"

# The request is in the property list now.  Leave nothing behind that the
# machine's compatibility shim would apply on top of it: the shim wins, by
# design, so an inherited variable would silently override this run.
unset QEMU_IPQ_FLASH_PERSIST QEMU_IPQ_FLASH_SIZE QEMU_IPQ_FLASH_JEDEC
unset QEMU_IPQ_NAND QEMU_IPQ_NAND_PERSIST QEMU_IPQ_NAND_SIZE QEMU_IPQ_NAND_ID
unset QEMU_IPQ_BOARD

# The switch port the netdev is wired to is a board property, not a QEMU one;
# port 5 is what the reference board calls lan1, port 1 is ether1/wan.  The
# boot strap word follows boot-device (SPI = 6) without being told, so a NOR
# boot needs nothing extra.
MACHINE="$MACHINE,eth-bridge-port=$IPQ_PORT"

# -kernel is a programming source, not a boot source: the machine boots the
# SBL1 ELF out of the configured die and only copies an image into a die that
# has no boot chain yet.  A die that already holds one ignores it, so passing it
# there only makes it look as if the dump were booted.  Blank, empty or non-ELF
# dies still get the image -- that is the "program this part" case.
die_has_elf() {
    [ -s "$1" ] || return 1
    [ "$(od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n')" = "7f454c46" ]
}

BOOTARGS=(-kernel "$DUMP")
BOOTNOTE=
case ${BOOT:-spi-nor} in
    spi-nor)
        if [ -n "$PERSIST" ] && die_has_elf "$PERSIST"; then
            BOOTARGS=()
            BOOTNOTE="
$DUMP was not programmed: $PERSIST already holds a boot chain (delete it to
program that die from this image)."
        fi ;;
    spi-nand)
        if [ -n "${NAND:-}" ] && die_has_elf "$NAND"; then
            BOOTARGS=()
            BOOTNOTE="
$DUMP was not programmed: $NAND already holds a boot chain."
        fi ;;
    qpic-nand)
        if [ -n "${QPIC:-}" ] && die_has_elf "$QPIC"; then
            BOOTARGS=()
            BOOTNOTE="
$DUMP was not programmed: $QPIC already holds a boot chain."
        fi ;;
esac

QEMU_ARGS=(-M "$MACHINE" "${BOOTARGS[@]}" -display none -monitor none
    -chardev "socket,id=con,host=$UART_BIND,port=$UART_PORT,server=on,wait=off,logfile=$LOG/console.log,logappend=on"
    -serial chardev:con
    -netdev "tap,id=net0,ifname=$TAP,script=no,downscript=no")

# --print: describe the run instead of performing it -- no root, no tap, no
# QEMU.  QEMU_IPQ_NETDEV is the one thing that still travels in the
# environment: it names which -netdev the emulated PHY's carrier belongs to.
if [ "$PRINT" = 1 ]; then
    printf 'QEMU_IPQ_NETDEV=net0 %s' "$PWD/build/qemu-system-arm"
    for a in "${QEMU_ARGS[@]}"; do
        case $a in
            *[!-A-Za-z0-9_./:=@%+,-]*) printf " '%s'" "$a" ;;
            *)                         printf ' %s' "$a" ;;
        esac
    done
    printf '\n'
    exit 0
fi

case $IPQ_PORT in
    0) PORTNAME="cpu port" ;;   1) PORTNAME="ether1 (wan)" ;;
    2) PORTNAME="lan4" ;;       3) PORTNAME="lan3" ;;
    4) PORTNAME="lan2" ;;       5) PORTNAME="lan1" ;;
    *) PORTNAME="port $IPQ_PORT" ;;
esac

# With NIC empty there is no bridge: the tap itself is the host's end of the
# cable.  With a NIC, the tap and the NIC share a bridge.
if [ -n "$NIC" ]; then HOSTIF=$BR; else HOSTIF=$TAP; fi

# A NIC that carries an address or the default route is somebody's uplink:
# enslaving it silently would cut that network off.
if [ -n "$NIC" ]; then
    ip link show "$NIC" >/dev/null 2>&1 || die "no interface $NIC"
    if [ "$FORCE" != 1 ] && { ip -4 addr show dev "$NIC" | grep -q "inet " ||
            ip route show default dev "$NIC" | grep -q .; }; then
        die "$NIC has an address or is the default route; set FORCE=1 if you
     really mean to move it into $BR"
    fi
fi

# Leftovers from a run that was killed hard.
[ -n "$NIC" ] && ip link set "$NIC" nomaster 2>/dev/null
ip link del "$TAP" 2>/dev/null
[ -n "$NIC" ] && ip link del "$BR" 2>/dev/null
pkill -9 -x qemu-system-arm 2>/dev/null

QEMU_PID= DHCP_PID= BYPASS_PID= CLEANED=0
cleanup() {
    [ "$CLEANED" = 1 ] && return
    CLEANED=1
    trap - EXIT INT TERM
    say ""
    say "lan1-bridge: tearing down"
    [ -n "$DHCP_PID" ] && kill "$DHCP_PID" 2>/dev/null
    [ -n "$BYPASS_PID" ] && kill "$BYPASS_PID" 2>/dev/null
    [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null
    wait 2>/dev/null
    ip link del "$TAP" 2>/dev/null
    [ -n "$NIC" ] && ip link set "$NIC" nomaster 2>/dev/null
    [ -n "$NIC" ] && ip link del "$BR" 2>/dev/null
    [ -n "$NIC" ] && ip link set "$NIC" down 2>/dev/null
    exit 0
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ------------------------------------------------------------------ the wire
if [ -n "$NIC" ]; then
    ip link add "$BR" type bridge || die "cannot create $BR"
    ip link set "$BR" up
    ip link set "$NIC" master "$BR" || die "cannot enslave $NIC"
    ip link set "$NIC" up
fi
ip tuntap add dev "$TAP" mode tap || die "cannot create $TAP (is /dev/net/tun there?)"
[ -n "$NIC" ] && { ip link set "$TAP" master "$BR" || die "cannot enslave $TAP"; }
ip link set "$TAP" up

rm -f "$LOG/host.log" "$LOG/console.log" "$LOG/dhcp.log"

# ------------------------------------------------------------------ the machine
QEMU_IPQ_NETDEV=net0 ./build/qemu-system-arm "${QEMU_ARGS[@]}" \
    > "$LOG/host.log" 2>&1 &
QEMU_PID=$!
sleep 0.3
kill -0 "$QEMU_PID" 2>/dev/null || { sed 's/^/  /' "$LOG/host.log" >&2; die "QEMU died"; }

# Nothing to arm: the machine itself skips the SBL1 -> QSEE handover (the TZ
# blob needs a protection fabric it does not model) by reading the stage
# entries out of the boot image, and it keeps doing that after every reset.
# The line to look for in $LOG/host.log is:
#   ipq4019: qsee: skip qsee@0x87e80000 -> appsbl@0x80000000 (...)
# QEMU_IPQ_QSEE=run lets QSEE execute instead (bring-up of the XPU model).
grep -aq "ipq4019: qsee: skip .* (QEMU_IPQ_QSEE=run keeps" "$LOG/host.log" ||
    say "lan1-bridge: note: no QSEE stage in this image; SBL1 will run it as-is"

# ------------------------------------------------------------------ host address
if [ "$DHCP" = 1 ] && command -v python3 >/dev/null &&
   [ -r "$PWD/packaging/dhcp_client.py" ]; then
    # The guest's DHCP server only exists once its userspace came up (~35 s on
    # the OpenWrt image), so keep asking: each attempt is short and the loop
    # covers the boot.  Keeping the address off the interface until a lease
    # arrives also proves the lease is what configured it.
    (
        for _ in $(seq 40); do
            ip -4 addr show dev "$HOSTIF" 2>/dev/null | grep -q "inet " && exit 0
            if lease=$(python3 "$PWD/packaging/dhcp_client.py" "$HOSTIF" 8 \
                             2>> "$LOG/dhcp.log"); then
                # shellcheck disable=SC2086
                eval "$lease"
                ip addr add "$ip/$prefix" dev "$HOSTIF" 2>/dev/null && exit 0
            fi
            sleep 1
        done
    ) &
    DHCP_PID=$!
else
    ip addr add "$STATIC_IP" dev "$HOSTIF" || die "cannot address $HOSTIF"
fi

for _ in $(seq 800); do
    ip -4 addr show dev "$HOSTIF" 2>/dev/null | grep -q "inet " && break
    sleep 0.1
done

HOSTADDR=$(ip -4 -br addr show "$HOSTIF" 2>/dev/null | awk '{$1="";$2="";print}' | xargs)
if [ -z "$HOSTADDR" ] && [ "$DHCP" = 1 ]; then
    say "lan1-bridge: the guest never offered a lease; falling back to $STATIC_IP"
    ip addr add "$STATIC_IP" dev "$HOSTIF" 2>/dev/null
    HOSTADDR=$(ip -4 -br addr show "$HOSTIF" 2>/dev/null | awk '{$1="";$2="";print}' | xargs)
fi
[ -n "$HOSTADDR" ] || HOSTADDR="(none)"

# ------------------------------------------------------------------ tell the user
if [ "$IS_MIKROTIK" = 1 ]; then
    WEBIP=192.168.88.1; WEBWHAT="RouterOS webfig"
else
    WEBIP=192.168.1.1;  WEBWHAT="OpenWrt LuCI"
fi

WIRE="$PORTNAME -> $TAP"
[ -n "$NIC" ] && WIRE="$WIRE -> $BR <- $NIC"

# Wait until the machine has described its buses before believing the log:
# QEMU starts immediately now (no -S), and a large persist file takes a moment
# to open, so the banner used to race the machine's own init lines.
for _ in $(seq 100); do
    grep -aq "^ipq4019: QGIC2" "$LOG/host.log" && break
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 0.1
done

BOOTWHO=$(grep -a "^ipq4019: loaded .* (SBL ELF)" "$LOG/host.log" | head -1 |
          sed 's/^ipq4019: loaded //; s/ (SBL ELF).*//')
case $BOOTWHO in
    "the NOR die")  BOOTLINE="the NOR die${PERSIST:+ ($PERSIST)}" ;;
    "the NAND die") BOOTLINE="the NAND die${NAND:+ ($NAND)}" ;;
    "")             BOOTLINE="(no SBL ELF found: see $LOG/host.log)" ;;
    *)              BOOTLINE="$BOOTWHO (programmed into the die)" ;;
esac

BOARDLINE=$(grep -a "^ipq4019: board " "$LOG/host.log" | head -1 |
            sed 's/^ipq4019: board //')
DIELINE=$(grep -a "^ipq4019: flash die " "$LOG/host.log" | head -1 |
          sed 's/^ipq4019: flash die //; s/, window.*//')
NORSTATE=
grep -aq "^ipq4019: nor store .* loaded (" "$LOG/host.log" && NORSTATE=loaded
grep -aq "^ipq4019: nor store .* written (a copy of the image)" "$LOG/host.log" &&
    NORSTATE="programmed from the image"
if [ -n "$PERSIST" ] && [ -z "$NORSTATE" ]; then
    die "--persist $PERSIST was requested, but the machine did not open it as
     the NOR die; see $LOG/host.log"
fi
if [ -n "$PERSIST" ]; then
    FLASH="  flash      die $DIELINE -> $PERSIST ($NORSTATE; Ctrl-C keeps it)"
else
    FLASH="  flash      die $DIELINE (RAM only: Ctrl-C restarts from the dump)"
fi

# The second die, reported from what the machine says it did.
NDIELINE=$(grep -a "^ipq4019: nand die " "$LOG/host.log" | head -1 |
           sed 's/^ipq4019: nand die //; s/, [0-9]* pages of .*//')
NSTATE=$(grep -a "^ipq4019: nand store " "$LOG/host.log" | head -1 |
         sed -n 's/.* \(loaded\|created, erased\) (.*/\1/p')
if [ -n "${NAND:-}" ] && [ -z "$NSTATE" ]; then
    die "--nand-persist $NAND was requested, but the machine did not open it as
     the NAND die; see $LOG/host.log"
fi
if [ -n "${NAND_ON:-}" ] && [ -z "$NDIELINE" ]; then
    die "a NAND was requested, but the machine put none on the SPI bus;
     see $LOG/host.log"
fi
if [ -n "${NAND:-}" ]; then
    NANDBANNER="  nand       die $NDIELINE -> $NAND ($NSTATE; Ctrl-C keeps it)"
elif [ -n "$NDIELINE" ]; then
    NANDBANNER="  nand       die $NDIELINE (RAM only)"
else
    NANDBANNER="  nand       absent (--nand puts a blank part on chip select 1)"
fi

cat >&2 <<EOF

  machine    $(basename "$DUMP")
  board      ${BOARDLINE:-see $LOG/host.log}
  boot       from $BOOTLINE
  switch     $WIRE
  host addr  $HOSTADDR on $HOSTIF
$FLASH
$NANDBANNER$BOOTNOTE

  UART0      115200 8N1 on $UART_BIND:$UART_PORT
              socat -,raw,echo=0 TCP:$UART_BIND:$UART_PORT
              nc $UART_BIND $UART_PORT
              transcript: $LOG/console.log

  Browser     http://$WEBIP   ($WEBWHAT; the guest also serves ssh on 22)

  Netboot     RouterBOOT's etherboot listens on ether1, not lan1:
                sudo IPQ_PORT=1 DHCP=0 ./lan1-bridge.sh
              then press Ctrl+E inside the 2 s window and run netinstall
              against $HOSTIF.

  Everything is removed again when this exits (Ctrl-C).  Logs: $LOG
EOF

wait "$QEMU_PID"
