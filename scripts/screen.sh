#!/bin/sh
# Save a PNG of a VM's screen.
#   scripts/screen.sh [OUT]        QEMU screendump (emulated display, e.g. VM 210)
#   scripts/screen.sh -g [OUT]     screencapture inside the guest over ssh; needed when the
#                                  display is a passed-through GPU (VM 211), which QEMU can't see
# OUT defaults to /tmp/ripple-screen.png.
set -eu
. "$(dirname "$0")/lib.sh"

inguest=0
[ "${1:-}" = -g ] && { inguest=1; shift; }
out=${1:-/tmp/ripple-screen.png}

if [ "$inguest" = 1 ]; then
    gssh 'f=/tmp/ripple-screen.png; screencapture -x -t png $f && cat $f && rm -f $f' > "$out"
else
    remote=/tmp/ripple-screen-$VMID.png
    hmp "screendump $remote -f png" >/dev/null
    scp -q "$RETRO:$remote" "$out"
    ssh "$RETRO" rm -f "$remote"
fi
echo "$out"
