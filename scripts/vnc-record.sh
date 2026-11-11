#!/bin/sh
# Record a VM's display in real time through QEMU's VNC server, and make an animated GIF that
# plays at the recorded speed (the README's animations). Recordings stay on dev.lan in the
# repo's tmp-ci/rec/NAME (raw frames, about 4 MB each) for making GIFs again with other settings.
#   VMID=210 scripts/vnc-record.sh rec NAME SECONDS [X,Y,W,H]   record (default: the whole screen)
#   scripts/vnc-record.sh gif NAME OUT.gif WIDTH [FROM TO [DITHER]]   make the GIF here
#   scripts/vnc-record.sh frame NAME SECONDS OUT.png               one recorded frame, to look at
# The connection takes about 1.5 s to set up: start `rec` in the background and send the event
# about 2 s later (scripts/qmp-input.sh). QEMU sends a frame only when the picture changed, and
# after a still spell it sends at most every 3 s and speeds up only step by step: run
# tools/ticker in the guest first (one pixel that changes, invisible in the GIF).
set -eu
. "$(dirname "$0")/lib.sh"

DEV=${DEV:-dev.lan}
REPO=Projects/moose-ripple
cmd=${1:?usage: vnc-record.sh rec|gif|frame NAME ...}
name=${2:?name}
dir=$REPO/tmp-ci/rec/$name
case "$cmd" in
rec)
    seconds=${3:?seconds}
    box=${4:-0,0,0,0}
    pw=$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 8)
    fifo=$(mktemp -u "/tmp/ripple-rec-$$.XXXXXX")
    mkfifo "$fifo"
    trap 'rm -f "$fifo"' EXIT
    ssh $SSH_OPTS "$DEV" "rm -rf $dir && mkdir -p $dir"
    rsync -a "$ROOT/tools/vncgrab/vncgrab.py" "$DEV:$dir/"
    ssh "$RETRO" "LC_PVE_TICKET=$pw qm vncproxy $VMID" <"$fifo" |
        ssh $SSH_OPTS "$DEV" "RECORD_BPP=${RECORD_BPP:-32} RECORD_ENCODING=${RECORD_ENCODING:-raw} VNC_PASSWORD=$pw ~/$REPO/.venv/bin/python -I $dir/vncgrab.py --record $seconds $box $dir" >"$fifo"
    ;;
gif)
    out=${3:?OUT.gif}
    width=${4:?width}
    range=${5:+$5 ${6:?TO}}
    dither=${7:+-d $7}
    rsync -a "$ROOT/tools/gifmake/" "$DEV:$REPO/tools/gifmake/"
    ssh $SSH_OPTS "$DEV" "cd ~/$REPO && cc -O2 -o tmp-ci/rec/gifmake tools/gifmake/gifmake.c -lm &&
        tmp-ci/rec/gifmake $dither tmp-ci/rec/$name tmp-ci/rec/$name.gif $width $range"
    rsync -a "$DEV:$REPO/tmp-ci/rec/$name.gif" "$out"
    echo "$out"
    ;;
frame)
    t=${3:?seconds}
    out=${4:?OUT.png}
    ssh $SSH_OPTS "$DEV" "~/$REPO/.venv/bin/python -I $dir/vncgrab.py --frame $dir $t $dir/frame.png"
    rsync -a "$DEV:$dir/frame.png" "$out"
    echo "$out"
    ;;
*)
    die "unknown command $cmd"
    ;;
esac
