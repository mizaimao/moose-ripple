#!/bin/sh
# The Windows 7 test VM on Hog (QEMU and KVM in the WSL2 distro moose-ripple, virglrenderer on
# Mesa's Direct3D 12 driver), driven from the workstation over ssh. docs/windows/test-vms.md,
# "On Hog".
#   tools/win7-debug/hog.sh start               the VM (/root/win7/start.sh, no window); it runs as
#                                               long as this does: WSL stops the distro when no
#                                               session is left, so run it in the background
#   tools/win7-debug/hog.sh stop|status         shut it down (and so end start), or ask
#   tools/win7-debug/hog.sh mon 'HMP COMMAND'   a QEMU monitor command, its answer printed
#   tools/win7-debug/hog.sh type [-n] 'TEXT'    typed (-n: then Return), as scripts/type.sh
#   tools/win7-debug/hog.sh key QCODE...        named keys, e.g. meta_l-r
#   tools/win7-debug/hog.sh click X Y           a left click at X,Y (0-32767 each, the tablet)
#   tools/win7-debug/hog.sh move X Y            the pointer to X,Y
#   tools/win7-debug/hog.sh shot OUT.png        the screen (QEMU's screendump, as PNG)
#   tools/win7-debug/hog.sh sh                  a root shell command on stdin, in the distro
# MIT licence (LICENSE at the repository root).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
HOG=${HOG:-hog}
VMDIR=/root/win7

wsl() { ssh "$HOG" "wsl -d moose-ripple -u root -- bash -s" 2>/dev/null; }

# QMP through netcat (the distro has no socat), as scripts/qmp-input.sh does on the rig
qmp_run() {
    { echo "set -eu"; echo "SOCK=$VMDIR/qmp.sock"; cat <<'EOF'
coproc Q { nc -U $SOCK; }
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
EOF
    cat; } | wsl
}

case "${1:-}" in
    start)
        # WSLg's compositor crashes over and over in a session without a desktop: stopped once
        # QEMU runs (it does not need it with SDL's offscreen driver)
        echo "cd $VMDIR && (sleep 5; pkill -x WSLGd) & cd $VMDIR && SDL_VIDEODRIVER=offscreen ./start.sh > qemu.log 2>&1 < /dev/null" | wsl ;;
    stop)
        echo "echo system_powerdown | nc -U -q 1 $VMDIR/monitor.sock >/dev/null; n=0; while pgrep -f win7-hog >/dev/null && [ \$n -lt 60 ]; do sleep 1; n=\$((n+1)); done; pkill -f win7-hog || true; echo stopped" | wsl ;;
    status)
        echo "pgrep -af win7-hog | cut -c1-60 || echo 'not running'" | wsl ;;
    mon)
        shift; printf 'echo %s | nc -U -q 1 %s/monitor.sock | tail -n +2\n' "'$*'" "$VMDIR" | wsl ;;
    type)
        shift
        TYPE_PRINT=1 "$root/scripts/type.sh" "$@" |
            { echo "d=\$(mktemp -d) && cd \$d && split -l 20 - c. 2>/dev/null <<'KEYS'"; cat; echo "KEYS"
              echo "for f in c.*; do nc -U -q 1 $VMDIR/monitor.sock < \$f >/dev/null; sleep 0.3; done; rm -rf \$d"; } | wsl ;;
    key)
        shift; for k in "$@"; do echo "sendkey $k"; done |
            { echo "nc -U -q 1 $VMDIR/monitor.sock >/dev/null <<'KEYS'"; cat; echo "KEYS"; } | wsl ;;
    click)
        printf 'ev "$(abs %s %s)"; sleep 0.2\nev %s; sleep 0.1\nev %s\n' "$2" "$3" \
            "'{\"type\":\"btn\",\"data\":{\"down\":true,\"button\":\"left\"}}'" \
            "'{\"type\":\"btn\",\"data\":{\"down\":false,\"button\":\"left\"}}'" | qmp_run ;;
    move)
        printf 'ev "$(abs %s %s)"\n' "$2" "$3" | qmp_run ;;
    shot)
        echo "echo 'screendump /tmp/rb-hog-shot.ppm' | nc -U -q 1 $VMDIR/monitor.sock >/dev/null; sleep 1; cat /tmp/rb-hog-shot.ppm; rm -f /tmp/rb-hog-shot.ppm" | wsl > "$2.ppm"
        sips -s format png "$2.ppm" --out "$2" >/dev/null 2>&1 && rm -f "$2.ppm" && echo "$2" ;;
    sh)
        wsl ;;
    *)
        sed -n '2,13p' "$0" >&2; exit 2 ;;
esac
