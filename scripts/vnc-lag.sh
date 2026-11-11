#!/bin/sh
# Display lag of a VM as seen through QEMU's VNC server, for displays QEMU's screendump can't
# read (virtio-gl) and setups where the pointer isn't in the picture (hardware cursor).
#   VMID=210 scripts/vnc-lag.sh [PRESSES] [KEY] [IDLE] [AREA]   defaults: 10 f12 2 strip
# Presses KEY (f9 Exposé or f12 Dashboard: each press opens or closes it) over the VNC
# connection itself, then takes frames back to back until the picture changes. AREA strip or
# strip:Y: 8 rows across the screen at mid-height or row Y, about 30 ms a frame; full: the screen
# without the menu bar, about 85 ms a frame at 1280x800 (Exposé). Timing is all on dev.lan,
# where tools/vncgrab/vncgrab.py --lag runs in the repo's .venv; the VNC frame time is printed
# first so it can be subtracted. Use an even PRESSES to leave the screen as it was. Connection
# as in scripts/screen-vnc.sh. Decision 0004: measured and reported, not a gate. Results:
# docs/re/qe-verification.md.
set -eu
. "$(dirname "$0")/lib.sh"

DEV=${DEV:-dev.lan}
presses=${1:-10} key=${2:-f12} idle=${3:-2} area=${4:-strip}
pw=$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 8)
tag=ripple-vnclag-$$
fifo=$(mktemp -u "/tmp/$tag.XXXXXX")
mkfifo "$fifo"
trap 'rm -f "$fifo"; ssh $SSH_OPTS "$DEV" "rm -f /tmp/$tag.py"' EXIT

scp -q $SSH_OPTS "$ROOT/tools/vncgrab/vncgrab.py" "$DEV:/tmp/$tag.py"
ssh "$RETRO" "LC_PVE_TICKET=$pw qm vncproxy $VMID" <"$fifo" |
    ssh $SSH_OPTS "$DEV" "VNC_PASSWORD=$pw ~/Projects/moose-ripple/.venv/bin/python -I /tmp/$tag.py --lag $presses $key $idle $area" >"$fifo"
