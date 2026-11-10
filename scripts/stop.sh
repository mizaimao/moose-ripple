#!/bin/sh
# Power off the dev VM. A clean shutdown panics in AppleSMC (docs/re/notebook.md), so this
# syncs the guest's disks over ssh and then stops the VM from Proxmox.
set -eu
. "$(dirname "$0")/lib.sh"

status=$(ssh "$RETRO" qm status "$VMID")
[ "$status" = "status: stopped" ] && { echo "VM $VMID already stopped"; exit 0; }
gssh 'sync' 2>/dev/null || echo "no ssh; stopping without a sync" >&2
sleep 2
ssh "$RETRO" qm stop "$VMID"
echo "VM $VMID stopped"
