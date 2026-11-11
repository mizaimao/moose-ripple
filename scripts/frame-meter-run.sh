#!/bin/sh
# WindowServer's own frame rate during the perf-run.sh scenarios, for any renderer (ours,
# Apple's software compositor on 210, the 9600 GT on 211: VMID=211). Copies build/frame-meter
# into the guest, then for each scenario runs it for 6 s while the scenario's keys go in.
#   scripts/frame-meter-run.sh [SCENARIO ...]     default: expose dashboard menu
set -eu
. "$(dirname "$0")/lib.sh"
[ $# -gt 0 ] || set -- expose dashboard menu
gssh 'cat > /tmp/ripple-frame-meter && chmod +x /tmp/ripple-frame-meter' < "$ROOT/build/frame-meter"
for s in "$@"; do
    case "$s" in
        expose)    keys='f9 f9 f9 f9' ;;
        dashboard) keys='f12 f12 f12 f12' ;;
        menu)      keys='ctrl-f2 right right right right left left left esc' ;;
        *)         die "unknown scenario $s" ;;
    esac
    ( sleep 0.5; for k in $keys; do ssh "$RETRO" "qm sendkey $VMID $k"; sleep 1.2; done ) &
    printf '%s: ' "$s"
    gssh '/tmp/ripple-frame-meter 6 0.25'
    wait
done
