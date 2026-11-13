#!/bin/sh
# Resolve a target/machine selection into one line per build group.
#
#   scripts/selected.sh "mips"                     all machines of mipsel
#   scripts/selected.sh "mipsel/*"                   same
#   scripts/selected.sh "mipsel/mt7621"              one machine
#   scripts/selected.sh "mipsel/mt7621,mips/mt7622"  explicit list
#   scripts/selected.sh "mipsel/*,arm/*"             whole architectures
#   scripts/selected.sh ""                         read .config (or everything)
#
# Output: one line per architecture, "arch|machine machine ...", so the caller
# builds one QEMU tree per executable and includes every selected machine in it.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"

specs=${1:-}

# No explicit selection: use .config, then fall back to everything.
if [ -z "$specs" ] && [ -f .config ]; then
    machines=$(sed -n 's/^CONFIG_MACHINE_\([A-Za-z0-9_]*\)=y$/\1/p' .config)
    if [ -n "$machines" ]; then
        specs=""
        for m in $machines; do
            found=""
            for t in $(ls targets); do
                [ -d "targets/$t/machines/$m" ] && found="$t"
            done
            [ -n "$found" ] || {
                echo "selected.sh: .config selects unknown machine '$m'" >&2
                exit 1
            }
            specs="${specs:+$specs,}$found/$m"
        done
    else
        targets=$(sed -n 's/^CONFIG_TARGET_\([A-Za-z0-9_]*\)=y$/\1/p' .config)
        [ -n "$targets" ] || targets=$(ls targets)
        specs=""
        for t in $targets; do specs="${specs:+$specs,}$t/*"; done
    fi
fi

if [ -z "$specs" ]; then
    specs=""
    for t in $(ls targets); do specs="${specs:+$specs,}$t/*"; done
fi

# Resolve each spec to "arch machine" lines.
pairs=$(
    old_ifs=$IFS; IFS=,
    for spec in $specs; do
        IFS=$old_ifs
        arch=${spec%%/*}
        mach=${spec#*/}
        [ "$mach" = "$spec" ] && mach='*'
        [ -d "targets/$arch" ] || {
            echo "selected.sh: unknown target '$arch'" >&2
            exit 1
        }
        if [ "$mach" = '*' ] || [ -z "$mach" ]; then
            for m in $(ls "targets/$arch/machines" 2>/dev/null); do
                echo "$arch $m"
            done
        else
            [ -d "targets/$arch/machines/$mach" ] || {
                echo "selected.sh: target '$arch' has no machine '$mach'" >&2
                exit 1
            }
            echo "$arch $mach"
        fi
        IFS=,
    done
)

echo "$pairs" | awk '
    NF == 0 { next }
    { if (!($1 in first)) { order[++n] = $1; first[$1] = 1 }
      if (!(($1 " " $2) in seen)) { seen[$1 " " $2] = 1; list[$1] = list[$1] " " $2 } }
    END { for (i = 1; i <= n; i++) print order[i] "|" list[order[i]] }
'
