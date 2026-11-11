#!/bin/sh
# Send keys, pointer moves and clicks to a VM through one QMP session on the rig.
#   VMID=210 scripts/qmp-input.sh "key f12" "sleep 2" "click 358 32193" "move 12467 30105"
# key QCODE | move X Y | click X Y (left button) | sleep SECONDS. X and Y are absolute,
# 0..32767 across the screen, and need a pointer driver in the guest (RippleQEMUTablet on 210;
# on VM 212, without one, a click test was followed by a guest hang). Don't run `qm sendkey`
# while this runs: QMP serves one client at a time here, and qm times out.
# See docs/re/qe-verification.md (Dashboard ripple).
set -eu
. "$(dirname "$0")/lib.sh"

args=
for a in "$@"; do args="$args '$a'"; done
ssh "$RETRO" "VMID=$VMID bash -s -- $args" <<'EOF'
set -eu
coproc Q { socat - UNIX-CONNECT:/var/run/qemu-server/$VMID.qmp; }
read -r _ <&"${Q[0]}"
qmp() {
    echo "$1" >&"${Q[1]}"
    local l
    while read -r l <&"${Q[0]}"; do
        case "$l" in *'"return"'*) return 0;; *'"error"'*) echo "$l" >&2; return 0;; esac
    done
}
ev() { qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[$1]}}"; }
abs() { echo "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$1}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$2}}"; }
qmp '{"execute":"qmp_capabilities"}'
for a in "$@"; do
    set -- $a
    case "$1" in
        key)   qmp "{\"execute\":\"send-key\",\"arguments\":{\"keys\":[{\"type\":\"qcode\",\"data\":\"$2\"}]}}" ;;
        move)  ev "$(abs "$2" "$3")" ;;
        click) ev "$(abs "$2" "$3")"; sleep 0.2
               ev '{"type":"btn","data":{"down":true,"button":"left"}}'; sleep 0.1
               ev '{"type":"btn","data":{"down":false,"button":"left"}}' ;;
        sleep) sleep "$2" ;;
        *)     echo "qmp-input: unknown action: $a" >&2; exit 2 ;;
    esac
done
kill "$Q_PID" 2>/dev/null || true
EOF
