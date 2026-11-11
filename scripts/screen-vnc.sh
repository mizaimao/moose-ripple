#!/bin/sh
# Save a PNG of what a VM's display really shows, read from QEMU's VNC server on the rig.
# Works where QEMU's screendump can't (vga: virtio-gl once the guest scans out: "no surface").
#   VMID=210 scripts/screen-vnc.sh [OUT]      OUT defaults to /tmp/ripple-screen-vnc.png
# Runs tools/vncgrab/vncgrab.py on the rig (standard Python, nothing installed) through
# `qm vncproxy`, with a one-off random VNC password that Proxmox expires after 30 s.
set -eu
. "$(dirname "$0")/lib.sh"

out=${1:-/tmp/ripple-screen-vnc.png}
pw=$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 8)
ssh "$RETRO" "LC_PVE_TICKET=$pw python3 -I - $VMID" <"$ROOT/tools/vncgrab/vncgrab.py" >"$out.tmp"
mv "$out.tmp" "$out"
echo "$out"
