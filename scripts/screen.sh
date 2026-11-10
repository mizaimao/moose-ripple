#!/bin/sh
# Save a PNG of the dev VM's screen (QEMU screendump), headless.
#   scripts/screen.sh [OUT]        default OUT: /tmp/ripple-screen.png
set -eu
. "$(dirname "$0")/lib.sh"

out=${1:-/tmp/ripple-screen.png}
remote=/tmp/ripple-screen-$VMID.png
hmp "screendump $remote -f png" >/dev/null
scp -q "$RETRO:$remote" "$out"
ssh "$RETRO" rm -f "$remote"
echo "$out"
