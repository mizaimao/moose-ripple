#!/bin/sh
# Record a VM's display in real time through QEMU's VNC server, and make an animated GIF that
# plays at the recorded speed (the README's animations). Recordings stay on dev.lan in the
# repo's tmp-ci/rec/NAME (raw frames, about 4 MB each) for making GIFs again with other settings.
#   VMID=210 scripts/vnc-record.sh rec NAME SECONDS [X,Y,W,H]   record (default: the whole screen)
#   VMID=210 scripts/vnc-record.sh host NAME SECONDS            record on the VM host itself
#     (tools/vncgrab/vncrec.c, built static on dev.lan, run from the host's /tmp and removed):
#     whole-screen changes arrive at QEMU's 30 ms pace; through the ssh relay of `rec` they took
#     50-250 ms each, and a fast animation came out jumpy.
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
host)
    seconds=${3:?seconds}
    pw=$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 8)
    tag=ripple-vncrec-$$
    ssh $SSH_OPTS "$DEV" "cd ~/$REPO && mkdir -p tmp-ci/rec && cc -O2 -static -o tmp-ci/rec/vncrec tools/vncgrab/vncrec.c && cat tmp-ci/rec/vncrec" |
        ssh "$RETRO" "cat > /tmp/$tag && chmod 700 /tmp/$tag && mkdir /tmp/$tag.d"
    # Straight to QEMU's VNC socket (qm vncproxy's relay held whole-screen frames to 120 ms):
    # a one-off password over QMP, as qm vncproxy sets one, expired again right after.
    ssh "$RETRO" "VMID=$VMID PW=$pw SECS=$seconds TAG=$tag bash -s" <<'EOS'
set -eu
qmp() { printf '%s\n' '{"execute":"qmp_capabilities"}' "$1" | socat -t 2 - UNIX-CONNECT:/var/run/qemu-server/$VMID.qmp >/dev/null; }
qmp "{\"execute\":\"set_password\",\"arguments\":{\"protocol\":\"vnc\",\"password\":\"$PW\"}}"
# An earlier expiry (qm vncproxy expires its password after 30 s) would still apply.
qmp '{"execute":"expire_password","arguments":{"protocol":"vnc","time":"+60"}}'
VNC_SOCKET=/var/run/qemu-server/$VMID.vnc VNC_PASSWORD=$PW /tmp/$TAG $SECS /tmp/$TAG.d
qmp '{"execute":"expire_password","arguments":{"protocol":"vnc","time":"now"}}'
EOS
    # The size line goes first in times.txt (tools/gifmake).
    ssh $SSH_OPTS "$DEV" "rm -rf $dir && mkdir -p $dir"
    ssh "$RETRO" "cd /tmp/$tag.d && cat size.txt times.txt > times.all && mv times.all times.txt &&
        tar -cf - frames.bin times.txt | zstd -1 -T0 -q" |
        ssh $SSH_OPTS "$DEV" "zstd -d -q | tar -C $dir -xf -"
    ssh "$RETRO" "rm -f /tmp/$tag /tmp/$tag.d/frames.bin /tmp/$tag.d/times.txt /tmp/$tag.d/size.txt && rmdir /tmp/$tag.d"
    rsync -a "$ROOT/tools/vncgrab/vncgrab.py" "$DEV:$dir/"
    ;;
gif)
    out=${3:?OUT.gif}
    width=${4:?width}
    range=${5:+$5 ${6:?TO}}
    dither=${7:+-d $7}
    loss=${GIF_LOSS:+-l $GIF_LOSS}
    rsync -a "$ROOT/tools/gifmake/" "$DEV:$REPO/tools/gifmake/"
    ssh $SSH_OPTS "$DEV" "cd ~/$REPO && cc -O2 -o tmp-ci/rec/gifmake tools/gifmake/gifmake.c -lm &&
        tmp-ci/rec/gifmake $dither $loss tmp-ci/rec/$name tmp-ci/rec/$name.gif $width $range"
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
