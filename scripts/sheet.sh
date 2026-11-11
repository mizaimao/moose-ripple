#!/bin/sh
# One contact-sheet PNG from captured frames (tools/vncgrab/sheet.py), made on dev.lan: the
# laptop only edits. Frames go to a scratch folder there and are removed afterwards.
#   scripts/sheet.sh OUT.png [-s SCALE] [-c COLUMNS] [-x X,Y,W,H] FRAME.png [FRAME.png ...]
#   scripts/sheet.sh -p X,Y[:X,Y...] FRAME.png [...]    pixel values instead (pngpix.py)
set -eu
. "$(dirname "$0")/lib.sh"

DEV=${DEV:-dev.lan}
tag=tmp-sheet-$$
remote=Projects/moose-ripple/$tag
if [ "$1" = -p ]; then
    points=$(echo "$2" | tr ':' ' ')
    shift 2
    ssh $SSH_OPTS "$DEV" "mkdir -p $remote"
    rsync -a "$@" "$DEV:$remote/"
    names=
    for f in "$@"; do names="$names $tag/$(basename "$f")"; done
    ssh $SSH_OPTS "$DEV" "cd ~/Projects/moose-ripple && .venv/bin/python -I tools/vncgrab/pngpix.py $points -- $names; rm -rf $tag"
    exit 0
fi
out=$1
shift
opts=
while [ $# -gt 0 ]; do
    case "$1" in
        -s|-c|-x) opts="$opts $1 $2"; shift 2 ;;
        *) break ;;
    esac
done
ssh $SSH_OPTS "$DEV" "mkdir -p $remote"
rsync -a "$ROOT/tools/vncgrab/" "$DEV:Projects/moose-ripple/tools/vncgrab/"
rsync -a "$@" "$DEV:$remote/"
names=
for f in "$@"; do names="$names $tag/$(basename "$f")"; done
ssh $SSH_OPTS "$DEV" "cd ~/Projects/moose-ripple && .venv/bin/python -I tools/vncgrab/sheet.py $opts $tag/sheet-out.png $names && cat $tag/sheet-out.png; rm -rf $tag" > "$out"
echo "$out"
