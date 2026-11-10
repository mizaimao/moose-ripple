#!/bin/sh
# Kernel debugging: QEMU's gdbstub on the retro rig, lldb on dev.lan with the 10.6.8 KDK.
#   scripts/debug.sh start           open the gdbstub (127.0.0.1:1234 on the rig) and tunnel it
#                                    to dev.lan's 127.0.0.1:1234 through this machine
#   scripts/debug.sh lldb CMD...     run lldb on dev.lan in batch mode: KDK kernel loaded,
#                                    attached, then each CMD as an -o command, then detach
#   scripts/debug.sh stop            close the gdbstub and the tunnels
# Attaching halts the whole VM until lldb detaches or continues.
# dev.lan has no ssh access to the rig, so both tunnels run from here:
#   dev.lan:1234 -> (ssh -R) -> here:12340 -> (ssh -L) -> rig:1234 (QEMU)
set -eu
. "$(dirname "$0")/lib.sh"

DEV=${DEV:-dev.lan}
KDK=${KDK:-.tools/kdk/10.6.8-10K549}
ARCH=${ARCH:-x86_64}          # the guest kernel being debugged: x86_64 or i386
HERE_PORT=12340

case "${1:-}" in
start)
    hmp "gdbserver tcp:127.0.0.1:1234" | grep -v '^qm>' || true
    pkill -f "127.0.0.1:$HERE_PORT:127.0.0.1:1234" 2>/dev/null || true
    pkill -f "127.0.0.1:1234:127.0.0.1:$HERE_PORT" 2>/dev/null || true
    ssh -f -N -o ExitOnForwardFailure=yes -L "127.0.0.1:$HERE_PORT:127.0.0.1:1234" "$RETRO"
    ssh -f -N -o ExitOnForwardFailure=yes -R "127.0.0.1:1234:127.0.0.1:$HERE_PORT" "$DEV"
    echo "gdbstub open; dev.lan 127.0.0.1:1234 reaches VM $VMID"
    ;;
lldb)
    shift
    opts=""
    for c in "$@"; do
        opts="$opts -o '$(printf '%s' "$c" | sed "s/'/'\\\\''/g")'"
    done
    # Never let lldb end the session with a kill: QEMU's gdbstub obeys it by exiting, which
    # powers the VM off (happened 2026-11-10). In batch mode an unexpected stop skips the
    # remaining -o commands and runs the -k ones, so both paths end in a detach.
    ssh "$DEV" "cd ~/Projects/moose-ripple && lldb --batch \
        -o 'target create --arch $ARCH $KDK/mach_kernel' \
        -o 'command script import tools/lldb/ripple_lldb.py' \
        -o 'gdb-remote 127.0.0.1:1234' $opts -o 'process detach' \
        -k 'bt 8' -k 'process detach'"
    ;;
stop)
    hmp "gdbserver none" | grep -v '^qm>' || true
    pkill -f "127.0.0.1:$HERE_PORT:127.0.0.1:1234" 2>/dev/null || true
    pkill -f "127.0.0.1:1234:127.0.0.1:$HERE_PORT" 2>/dev/null || true
    echo "gdbstub closed"
    ;;
*)
    die "usage: $0 start | lldb CMD... | stop"
    ;;
esac
