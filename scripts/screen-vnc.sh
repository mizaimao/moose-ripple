#!/bin/sh
# Save a PNG of what a VM's display really shows, read from QEMU's VNC server on the rig.
# Works where QEMU's screendump can't (vga: virtio-gl once the guest scans out: "no surface").
#   VMID=210 scripts/screen-vnc.sh [OUT]      OUT defaults to /tmp/ripple-screen-vnc.png
#   VMID=210 scripts/screen-vnc.sh -n 12 -i 0.15 OUT.png
#                     12 frames on one connection, one every 0.15 s or as fast as they come
#                     (raw 1280x800 is 4 MB a frame), saved as OUT-00.png..OUT-11.png. The
#                     connection takes about 1.5 s to set up: start it that long before the
#                     event (e.g. in the background, then click with scripts/qmp-input.sh).
# The client, tools/vncgrab/vncgrab.py, runs on dev.lan in the repo's .venv (standard library
# only). This script joins its stdin/stdout to `qm vncproxy` on the rig through a FIFO here,
# with a one-off random VNC password that Proxmox expires after 30 s. Nothing runs or is
# installed on the rig besides qm.
set -eu
. "$(dirname "$0")/lib.sh"

DEV=${DEV:-dev.lan}
n=1 i=0
while [ $# -gt 0 ]; do
    case "$1" in
        -n) n=$2; shift 2 ;;
        -i) i=$2; shift 2 ;;
        *)  break ;;
    esac
done
out=${1:-/tmp/ripple-screen-vnc.png}
pw=$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 8)
tag=ripple-vnc-$$
fifo=$(mktemp -u "/tmp/$tag.XXXXXX")
mkfifo "$fifo"
trap 'rm -f "$fifo"' EXIT

scp -q "$ROOT/tools/vncgrab/vncgrab.py" "$DEV:/tmp/$tag.py"
ssh "$RETRO" "LC_PVE_TICKET=$pw qm vncproxy $VMID" <"$fifo" |
    ssh "$DEV" "mkdir -p /tmp/$tag.d && VNC_PASSWORD=$pw ~/Projects/moose-ripple/.venv/bin/python -I /tmp/$tag.py -n $n -i $i /tmp/$tag.d/f.png" >"$fifo"
if [ "$n" = 1 ]; then
    ssh "$DEV" "cat /tmp/$tag.d/f.png; rm -rf /tmp/$tag.d /tmp/$tag.py" >"$out.tmp"
    mv "$out.tmp" "$out"
    echo "$out"
else
    stem=${out%.png}
    ssh "$DEV" "cd /tmp/$tag.d && tar cf - f-*.png; rm -rf /tmp/$tag.d /tmp/$tag.py" |
        tar xf - -C "$(dirname "$stem")" -s "|^f-|$(basename "$stem")-|"
    k=0; while [ $k -lt "$n" ]; do printf "%s-%02d.png\n" "$stem" $k; k=$((k + 1)); done
fi
