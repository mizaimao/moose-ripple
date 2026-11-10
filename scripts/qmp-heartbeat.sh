#!/bin/sh
# Watch QEMU's main loop for stalls: send QMP query-status every 20 ms for SECONDS and report
# replies that took longer than 50 ms (QMP is served by the main loop, which also runs device
# emulation such as virtio-gpu commands and the USB controllers).
#   scripts/qmp-heartbeat.sh [SECONDS]        default 30; runs on the retro rig
set -eu
. "$(dirname "$0")/lib.sh"

SECONDS_TO_RUN=${1:-30}
ssh "$RETRO" "VMID=$VMID DUR=$SECONDS_TO_RUN bash -s" <<'EOF'
set -eu
coproc Q { socat - UNIX-CONNECT:/var/run/qemu-server/$VMID.qmp; }
read -r _ <&"${Q[0]}"
qmp() {
    echo "$1" >&"${Q[1]}"
    local line
    while read -r line <&"${Q[0]}"; do
        case "$line" in *'"return"'*|*'"error"'*) return 0;; esac
    done
}
qmp '{"execute":"qmp_capabilities"}'
end=$(( $(date +%s) + DUR )); n=0; slow=0; worst=0
while [ "$(date +%s)" -lt "$end" ]; do
    t0=$(date +%s%N)
    qmp '{"execute":"query-status"}'
    dt=$(( ($(date +%s%N) - t0) / 1000000 ))
    n=$((n + 1))
    [ "$dt" -gt "$worst" ] && worst=$dt
    if [ "$dt" -gt 50 ]; then slow=$((slow + 1)); echo "$(date +%T.%3N) stall ${dt} ms"; fi
    sleep 0.02
done
echo "$n probes, $slow over 50 ms, worst ${worst} ms"
EOF
