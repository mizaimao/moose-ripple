#!/bin/sh
# Display lag: how long a pointer move takes to reach QEMU's display surface.
#   scripts/lag.sh [ROUNDS] [IDLE-SECONDS]      defaults: 10 rounds, 2 s idle before each move
# Runs on the retro rig over one QMP connection: screendump (PPM) as the baseline, move the
# pointer with input-send-event (absolute), then screendump in a loop until the image differs.
# Needs the software cursor (the default) so the pointer is part of the image. The result
# includes one screendump's time, printed first so it can be subtracted. VNC consoles
# (Proxmox) add their own delay on top, which this doesn't measure.
set -eu
. "$(dirname "$0")/lib.sh"

ROUNDS=${1:-10}
IDLE=${2:-2}
ssh "$RETRO" "VMID=$VMID ROUNDS=$ROUNDS IDLE=$IDLE bash -s" <<'EOF'
set -eu
F=/tmp/ripple-lag-$VMID.ppm
coproc Q { socat - UNIX-CONNECT:/var/run/qemu-server/$VMID.qmp; }
read -r _ <&"${Q[0]}"                                  # greeting
qmp() {                                                # send one command, wait for its reply
    echo "$1" >&"${Q[1]}"
    local line
    while read -r line <&"${Q[0]}"; do
        case "$line" in *'"return"'*|*'"error"'*) return 0;; esac
    done
}
shot() { qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$F\"}}"; md5sum "$F" | cut -c1-32; }
move() { qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$1}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$2}}]}}"; }
ms() { echo $(( ($(date +%s%N) - $1) / 1000000 )); }

qmp '{"execute":"qmp_capabilities"}'
t=$(date +%s%N); for i in 1 2 3 4 5; do shot >/dev/null; done
echo "one screendump: $(( $(ms $t) / 5 )) ms"
out=""
for i in $(seq 1 "$ROUNDS"); do
    sleep "$IDLE"
    base=$(shot)
    x=$(( 8192 + (i % 2) * 16384 ))
    t0=$(date +%s%N)
    move "$x" 16384
    n=0
    while [ "$(shot)" = "$base" ]; do
        n=$((n + 1))
        [ "$n" -lt 500 ] || { echo "round $i: no change seen"; break; }
    done
    out="$out $(ms "$t0")"
done
echo "lag (ms, idle ${IDLE}s before each move):$out"
rm -f "$F"
EOF
