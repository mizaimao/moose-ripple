#!/bin/sh
# The Windows 7 test VM in UTM on macserver (ripple-win7: x86-64 emulated on Apple silicon,
# virglrenderer behind UTM's QEMU), driven from the workstation over ssh through the QMP socket
# the VM's settings add. docs/windows/test-vms.md, "In UTM".
#   tools/win7-debug/utm.sh start|stop|status   the VM (utmctl)
#   tools/win7-debug/utm.sh mon 'HMP COMMAND'   a QEMU monitor command
#   tools/win7-debug/utm.sh type [-n] 'TEXT'    typed (-n: then Return), as scripts/type.sh
#   tools/win7-debug/utm.sh key QCODE...        named keys, e.g. meta_l-r
#   tools/win7-debug/utm.sh click X Y           a left click at X,Y (0-32767 each, the tablet)
#   tools/win7-debug/utm.sh shot OUT.png        the screen (QEMU's screendump; only until our
#                                               driver owns the display)
# MIT licence (LICENSE at the repository root).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
MAC=${MAC:-macserver.lan}
VM=${VM:-ripple-win7}
SOCK='$HOME/Library/Group Containers/WDNLXAD4W8.com.utmapp.UTM/rb-qmp-w7.sock'
UTMCTL=/Applications/UTM.app/Contents/MacOS/utmctl

# QMP lines on stdin, each sent after the last; the answers are read until a pause
qmp() {
    { echo '{"execute":"qmp_capabilities"}'; while IFS= read -r l; do printf '%s\n' "$l"; done; } |
        ssh "$MAC" "{ while IFS= read -r l; do printf '%s\n' \"\$l\"; sleep 0.05; done; sleep 1; } | nc -U \"$SOCK\""
}
hmp() { printf '{"execute":"human-monitor-command","arguments":{"command-line":"%s"}}\n' "$1"; }
ev() { printf '{"execute":"input-send-event","arguments":{"events":[%s]}}\n' "$1"; }
abs() { printf '{"type":"abs","data":{"axis":"x","value":%s}},{"type":"abs","data":{"axis":"y","value":%s}}' "$1" "$2"; }

case "${1:-}" in
    start|stop|status)
        ssh "$MAC" "$UTMCTL $1 $VM" ;;
    mon)
        hmp "$2" | qmp | grep -o '"return": *"[^"]*"' | sed 's/^"return": *"//; s/"$//; s/\\r\\n/\n/g' ;;
    type)
        shift
        TYPE_PRINT=1 "$root/scripts/type.sh" "$@" | while read -r c; do hmp "$c"; done | qmp >/dev/null ;;
    key)
        shift; for k in "$@"; do hmp "sendkey $k"; done | qmp >/dev/null ;;
    click)
        { ev "$(abs "$2" "$3")"; ev '{"type":"btn","data":{"down":true,"button":"left"}}'
          ev '{"type":"btn","data":{"down":false,"button":"left"}}'; } | qmp >/dev/null ;;
    shot)
        # Written where UTM's sandboxed QEMU may write: its group container
        dir="$(ssh "$MAC" 'echo $HOME')/Library/Group Containers/WDNLXAD4W8.com.utmapp.UTM"
        printf '{"execute":"screendump","arguments":{"filename":"%s/rb-utm-shot.ppm"}}\n' "$dir" | qmp >/dev/null
        ssh "$MAC" "cat \"$dir/rb-utm-shot.ppm\" 2>/dev/null; rm -f \"$dir/rb-utm-shot.ppm\"" > "$2.ppm"
        if [ -s "$2.ppm" ]; then sips -s format png "$2.ppm" --out "$2" >/dev/null 2>&1; echo "$2"; else echo "no picture (QEMU has no surface once our driver runs)"; fi
        rm -f "$2.ppm" ;;
    *)
        sed -n '2,12p' "$0" >&2; exit 2 ;;
esac
