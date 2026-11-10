#!/bin/sh
# Snapshot the dev VM before a kernel experiment (HANDOFF.md rule 2).
#   scripts/snapshot.sh            disk-only snapshot named pre-<date>-<time>
#   scripts/snapshot.sh NAME       disk-only snapshot named NAME
#   scripts/snapshot.sh -r NAME    also save RAM, so a rollback resumes the running VM
#   scripts/snapshot.sh -l         list snapshots
#   scripts/snapshot.sh -b NAME    roll back to NAME
set -eu
. "$(dirname "$0")/lib.sh"

vmstate=0
case "${1:-}" in
    -l) exec ssh "$RETRO" qm listsnapshot "$VMID" ;;
    -b) [ -n "${2:-}" ] || die "usage: $0 -b NAME"
        exec ssh "$RETRO" qm rollback "$VMID" "$2" ;;
    -r) vmstate=1; shift ;;
esac
name=${1:-pre-$(date +%Y%m%d-%H%M%S)}
ssh "$RETRO" qm snapshot "$VMID" "$name" --vmstate "$vmstate"
echo "snapshot $name taken (vmstate=$vmstate)"
