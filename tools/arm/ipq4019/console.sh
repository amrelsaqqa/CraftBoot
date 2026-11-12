#!/bin/bash
# console.sh -- boot a dump with UART0 on a pty, so an ordinary terminal
# program (picocom, minicom, screen, miniterm) can attach and detach like it
# would to a USB-serial adapter.
#
#   ./console.sh [options] [image] [tag]
#
#   --board=NAME       start from a board preset: default, none, mikrotik-952
#                      (default: the machine's own, i.e. board=default)
#   --persist[=FILE]   programs and erases go to FILE and survive Ctrl-C
#                      (default /tmp/<tag>-flash.bin); the file is the part
#   --nor-size=N       die size in bytes, e.g. 8388608 for the 8 MiB dumps
#                      (--flash-size= is the old spelling and still works)
#   --nor-id=ID        RDID of that part, e.g. ef4017 (or "ef 40 17"); the
#                      kernel sizes the flash from this ID, so it must match N
#                      (--flash-jedec= is the old spelling and still works)
#   --nand             put a SPI NAND behind chip select 1 as well (132 MiB,
#                      MX35LF1GE4AB); it is RAM unless --nand-persist is given
#   --nand-persist[=FILE]  programs and erases on that die go to FILE and
#                      survive Ctrl-C (default /tmp/<tag>-nand.bin)
#   --nand-size=N      NAND die bytes, a whole number of erase blocks
#   --nand-id=ID       NAND RDID bytes, e.g. c212
#   --boot=DEVICE      which die the boot chain is read from: spi-nor
#                      (default), spi-nand, none
#
# Every one of these is a -machine property, not an environment variable: the
# machine is configured, so the command line is the whole description of the
# board, and `./console.sh --persist=/tmp/x.bin` and
# `-M ipq4019,nor-file=/tmp/x.bin` are the same thing.  The old QEMU_IPQ_*
# variables are still read as the *request* (so an exported
# QEMU_IPQ_FLASH_PERSIST still means what it did), then translated and removed,
# because the machine's compatibility shim would otherwise win over the
# properties built here.
#
# Prints the pty to attach to and then just sits there running the machine.
# Everything the guest writes is also appended to the console log, including
# while nobody is attached (bytes written to a pty with no reader are dropped).
#
#   picocom -b 115200 /tmp/ipq4019.tty       # leave with Ctrl-A Ctrl-X
#
# QEMU's pty backend notices the client going away and re-arms its read watch
# when a client opens the slave again, so attaching/detaching is free; the
# guest keeps running either way.
#
# Without --persist the flash is RAM: a guest `reboot` keeps what was written,
# but quitting QEMU throws it away.  The banner shows which of the two the
# machine actually did, from QEMU's own log rather than from our intentions.
#
# The boot image is a *programming* source: the machine boots the SBL1 ELF out
# of the die and only copies the image into a die that has no boot chain yet
# (first run, or a blank part).  So the image is only passed as -kernel when it
# can be used -- a die that already holds one ignores it, and the banner's
# `Boot` line says which of the two actually ran.
#
# The SBL1 -> QSEE handover is skipped by the machine itself: it reads the
# stage entries out of the boot image and redirects in the CPU loop, so it
# keeps working after a `reboot` with no host-side agent attached.
set -u
cd "$(dirname "$0")"

usage() {
    sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//' >&2
}

# Options and the two positionals may be interleaved; anything that starts with
# a dash is ours, so an image called "-something" needs ./ .
BOARD=${QEMU_IPQ_BOARD:-}
PERSIST=${QEMU_IPQ_FLASH_PERSIST:-}
FLASH_SIZE=${QEMU_IPQ_FLASH_SIZE:-}
FLASH_JEDEC=${QEMU_IPQ_FLASH_JEDEC:-}
NAND=${QEMU_IPQ_NAND_PERSIST:-}
NAND_ON=${QEMU_IPQ_NAND:-}
NAND_SIZE=${QEMU_IPQ_NAND_SIZE:-}
NAND_ID=${QEMU_IPQ_NAND_ID:-}
BOOT=
POS=()
while [ $# -gt 0 ]; do
    case $1 in
        --board)          BOARD=${2:?--board needs a value}; shift 2 ;;
        --board=*)        BOARD=${1#*=}; shift ;;
        --persist)        PERSIST=default; shift ;;
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
        --nand-persist)   NAND_ON=1; NAND=default; shift ;;
        --nand-persist=*) NAND_ON=1; NAND=${1#*=}; shift ;;
        --nand-size)      NAND_SIZE=${2:?--nand-size needs a value}; shift 2 ;;
        --nand-size=*)    NAND_SIZE=${1#*=}; shift ;;
        --nand-id)        NAND_ID=${2:?--nand-id needs a value}; shift 2 ;;
        --nand-id=*)      NAND_ID=${1#*=}; shift ;;
        --boot)           BOOT=${2:?--boot needs a value}; shift 2 ;;
        --boot=*)         BOOT=${1#*=}; shift ;;
        -h|--help)        usage; exit 0 ;;
        -*)               echo "console.sh: unknown option $1" >&2; usage; exit 1 ;;
        *)                POS+=("$1"); shift ;;
    esac
done

IMG=${POS[0]:-$HOME/kt708-ipq-openwrt24.10.0-no-wifi.bin}
TAG=${POS[1]:-ipq4019}
[ "$PERSIST" = default ] && PERSIST=/tmp/$TAG-flash.bin
[ "$NAND" = default ] && NAND=/tmp/$TAG-nand.bin
if [ -n "$NAND" ]; then
    NAND_ON=1
fi
# "NAND=0" in the environment is a request for no NAND, which must not read as
# a non-empty string further down.
[ "${NAND_ON:-}" = 0 ] && NAND_ON=

# The dies are -machine values, and -machine splits on commas: a path with one
# in it cannot be expressed, so refuse it here instead of letting QEMU chop the
# property list into two.
for f in "$PERSIST" "$NAND"; do
    case $f in
        *,*) echo "console.sh: '$f' contains a comma, which -machine cannot carry" >&2
             exit 1 ;;
    esac
done

# The board, as one property list: preset first, then the explicitly given
# parts, which always win over it.
MACHINE=ipq4019
[ -n "$BOARD" ]       && MACHINE="$MACHINE,board=$BOARD"
# Naming the die's file is also a statement that the board has that die: with
# board=none, `--persist=x.bin` should mean "a NOR, backed by x.bin", not
# "nothing, plus a file nobody reads".
[ -n "$PERSIST" ]     && MACHINE="$MACHINE,nor=on"
[ -n "$PERSIST" ]     && MACHINE="$MACHINE,nor-file=$PERSIST"
[ -n "$FLASH_SIZE" ]  && MACHINE="$MACHINE,nor-size=$FLASH_SIZE"
[ -n "$FLASH_JEDEC" ] && MACHINE="$MACHINE,nor-id=$FLASH_JEDEC"
[ -n "$NAND_ON" ]     && MACHINE="$MACHINE,nand=on"
[ -n "$NAND" ]        && MACHINE="$MACHINE,nand-file=$NAND"
[ -n "$NAND_SIZE" ]   && MACHINE="$MACHINE,nand-size=$NAND_SIZE"
[ -n "$NAND_ID" ]     && MACHINE="$MACHINE,nand-id=$NAND_ID"
[ -n "$BOOT" ]        && MACHINE="$MACHINE,boot-device=$BOOT"

# The request is now in the property list.  Leave nothing behind that the
# machine's compatibility shim would apply on top of it (the shim wins, by
# design, so an inherited variable would silently override this run).
unset QEMU_IPQ_FLASH_PERSIST QEMU_IPQ_FLASH_SIZE QEMU_IPQ_FLASH_JEDEC
unset QEMU_IPQ_NAND QEMU_IPQ_NAND_PERSIST QEMU_IPQ_NAND_SIZE QEMU_IPQ_NAND_ID
unset QEMU_IPQ_BOARD

TTY=/tmp/$TAG.tty
LOG=${QEMU_IPQ_CONSOLE_LOG:-/tmp/$TAG-console.log}
HOSTLOG=/tmp/$TAG-host.log
rm -f "$TTY" "$LOG" "$HOSTLOG"

# -kernel is a programming source, not a boot source: the machine boots the SBL1
# ELF out of the configured die, and only copies an image into a die that has no
# boot chain yet.  So pass it exactly when it can be used -- a die that already
# holds the ELF would ignore it, and passing it anyway is what makes it look as
# if the dump were being booted.  A die that is blank, empty or not an ELF still
# gets the image, which is the "program this part" case.
die_has_elf() {
    [ -s "$1" ] || return 1
    [ "$(od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n')" = "7f454c46" ]
}

BOOTARGS=(-kernel "$IMG")
BOOTNOTE=
case ${BOOT:-spi-nor} in
    spi-nor)
        if [ -n "$PERSIST" ] && die_has_elf "$PERSIST"; then
            BOOTARGS=()
            [ -n "${POS[0]:-}" ] && BOOTNOTE="
$IMG was not programmed: $PERSIST already holds a boot chain, and a die is the
part, not a cache of the image.  Delete that file to program it from the dump."
        fi ;;
    spi-nand)
        if [ -n "$NAND" ] && die_has_elf "$NAND"; then
            BOOTARGS=()
            [ -n "${POS[0]:-}" ] && BOOTNOTE="
$IMG was not programmed: $NAND already holds a boot chain."
        fi ;;
esac

./build/qemu-system-arm -M "$MACHINE" "${BOOTARGS[@]}" -display none \
    -chardev "pty,id=con,path=$TTY,logfile=$LOG,logappend=on" \
    -serial chardev:con -monitor none >"$HOSTLOG" 2>&1 &
QP=$!
trap 'kill -9 $QP 2>/dev/null; rm -f "$TTY"' EXIT INT TERM

for _ in $(seq 50); do [ -e "$TTY" ] && break; sleep 0.1; done
PTY=$(readlink -f "$TTY" 2>/dev/null || true)

# The pty exists before the machine has finished initialising, so the log is
# not complete yet.  QGIC2 is printed once the buses are up (after the flash
# and NAND have been opened), which makes it the point where reading the log
# answers "what did the machine actually do" instead of "what has it printed
# so far".  Without this wait a large persist file made the NAND check fail on
# a machine that had opened it correctly.
for _ in $(seq 100); do
    grep -aq "^ipq4019: QGIC2" "$HOSTLOG" && break
    kill -0 $QP 2>/dev/null || break
    sleep 0.1
done
# A refusal is a "qemu-system-arm: ..." line, but warnings share the prefix and
# booting a die with no -kernel legitimately prints one ("no -bios: boot ROM at
# 0x0 is empty" -- the die is the boot ROM here).
if grep -a "^qemu-system-arm: " "$HOSTLOG" | grep -qav "warning: "; then
    echo "console.sh: QEMU refused to start:" >&2
    grep -a "^qemu-system-arm: " "$HOSTLOG" | grep -av "warning: " |
        head -2 | sed 's/^qemu-system-arm: //; s/^/  /' >&2
    exit 1
fi

# What did the machine actually do?  QEMU's own log answers that; a request that
# never arrived (a typo'd path, a variable from another shell, a QEMU too old to
# know the property) must not leave the session silently RAM-only while the user
# believes it is persistent.
#
# The die line carries its size, RDID and the window it was mapped into; the
# store line says whether the file that *is* the part was loaded from, created
# empty, or programmed from the image.
BOARDLINE=$(grep -a "^ipq4019: board " "$HOSTLOG" | head -1 |
            sed 's/^ipq4019: board //')
DIELINE=$(grep -a "^ipq4019: flash die " "$HOSTLOG" | head -1 |
          sed 's/^ipq4019: flash die //; s/, window.*//')
NORSTATE=
grep -aq "^ipq4019: nor store .* loaded (" "$HOSTLOG" && NORSTATE=loaded
grep -aq "^ipq4019: nor store .* written (a copy of the image)" "$HOSTLOG" &&
    NORSTATE="programmed from the image"
if [ -n "$PERSIST" ] && [ -z "$NORSTATE" ]; then
    echo "console.sh: --persist $PERSIST was requested, but the machine did" \
         "not open it as the NOR die; see $HOSTLOG" >&2
    exit 1
fi
if [ -n "$PERSIST" ]; then
    FLASH="die $DIELINE -> $PERSIST ($NORSTATE)"
    NOTE="
Ctrl-C is a power cut, not a scratch session: $PERSIST is the part, so anything
the guest programmed or erased is already in it.  Delete that file for a fresh die."
else
    FLASH="die $DIELINE (RAM only)"
    NOTE="
Flash is RAM only (no --persist): a guest reboot keeps what was written, quitting
QEMU does not.  Re-run with --persist[=/path] to keep it."
fi

# The second die is reported the same way: from what the machine says it did.
NDIELINE=$(grep -a "^ipq4019: nand die " "$HOSTLOG" | head -1 |
           sed 's/^ipq4019: nand die //; s/, [0-9]* pages of .*//')
NSTATE=$(grep -a "^ipq4019: nand store " "$HOSTLOG" | head -1 |
         sed -n 's/.* \(loaded\|created, erased\) (.*/\1/p')
if [ -n "$NAND" ] && [ -z "$NSTATE" ]; then
    echo "console.sh: --nand-persist $NAND was requested, but the machine did" \
         "not open it as the NAND die; see $HOSTLOG" >&2
    exit 1
fi
if [ -n "$NAND_ON" ] && [ -z "$NDIELINE" ]; then
    echo "console.sh: a NAND was requested, but the machine put none on the" \
         "SPI bus; see $HOSTLOG" >&2
    exit 1
fi
if [ -n "$NAND" ]; then
    NANDLINE="die $NDIELINE -> $NAND ($NSTATE)"
    NNOTE="
The NAND (chip select 1) is the same contract: $NAND is that part."
elif [ -n "$NDIELINE" ]; then
    NANDLINE="die $NDIELINE (RAM only)"
    NNOTE="
The NAND (chip select 1) is RAM only: --nand-persist[=/path] keeps it."
else
    NANDLINE="absent (add --nand for a SPI NAND on chip select 1)"
    NNOTE=""
fi
NOTE="$NOTE$NNOTE$BOOTNOTE"

# Which one actually booted: the machine names it in its own log line.
BOOTWHO=$(grep -a "^ipq4019: loaded .* (SBL ELF)" "$HOSTLOG" | head -1 |
          sed 's/^ipq4019: loaded //; s/ (SBL ELF).*//')
case $BOOTWHO in
    "the NOR die")  BOOTLINE="from the NOR die${PERSIST:+ ($PERSIST)}" ;;
    "the NAND die") BOOTLINE="from the NAND die${NAND:+ ($NAND)}" ;;
    "")             BOOTLINE="(no SBL ELF found: see $HOSTLOG)" ;;
    *)              BOOTLINE="from $BOOTWHO (programmed into the die)" ;;
esac

cat <<EOF
UART0 is on $TTY${PTY:+ -> $PTY}   (115200 8N1, no flow control)

  picocom -b 115200 $TTY        leave picocom with Ctrl-A Ctrl-X
  minicom -D $TTY -b 115200
  screen $PTY 115200

Guest output is also logged to $LOG (complete, even while detached).
Board      ${BOARDLINE:-(see $HOSTLOG)}
Boot       ${BOOTLINE}
Flash      ${FLASH:-(see $HOSTLOG)}
NAND       ${NANDLINE}
Rebooting the guest works: the machine redirects every boot's SBL1 -> QSEE
handover itself, at the console.
QSEE redirects are logged to $HOSTLOG ("ipq4019: qsee: skip ...").
QEMU's own messages go to $HOSTLOG.  Ctrl-C quits the machine.$NOTE
EOF

wait $QP
