#!/bin/sh
# Save a PNG of what a VM's display really shows, read from QEMU's VNC server on the rig.
# Works where QEMU's screendump can't (vga: virtio-gl once the guest scans out: "no surface").
#   VMID=210 scripts/screen-vnc.sh [OUT]      OUT defaults to /tmp/ripple-screen-vnc.png
# The client, tools/vncgrab/vncgrab.py, runs on dev.lan in the repo's .venv (standard library
# only). This script joins its stdin/stdout to `qm vncproxy` on the rig through a FIFO here,
# with a one-off random VNC password that Proxmox expires after 30 s. Nothing runs or is
# installed on the rig besides qm.
set -eu
. "$(dirname "$0")/lib.sh"

DEV=${DEV:-dev.lan}
out=${1:-/tmp/ripple-screen-vnc.png}
pw=$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 8)
tag=ripple-vnc-$$
fifo=$(mktemp -u "/tmp/$tag.XXXXXX")
mkfifo "$fifo"
trap 'rm -f "$fifo"' EXIT

scp -q "$ROOT/tools/vncgrab/vncgrab.py" "$DEV:/tmp/$tag.py"
ssh "$RETRO" "LC_PVE_TICKET=$pw qm vncproxy $VMID" <"$fifo" |
    ssh "$DEV" "VNC_PASSWORD=$pw ~/Projects/moose-ripple/.venv/bin/python -I /tmp/$tag.py /tmp/$tag.png" >"$fifo"
ssh "$DEV" "cat /tmp/$tag.png; rm -f /tmp/$tag.png /tmp/$tag.py" >"$out.tmp"
mv "$out.tmp" "$out"
echo "$out"
