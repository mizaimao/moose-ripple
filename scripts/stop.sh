#!/bin/sh
# Shut the dev VM down cleanly (`shutdown -h now` in the guest), and power it off from
# Proxmox only if the guest doesn't stop within 60 s or can't be reached.
# (Clean shutdowns used to panic in AppleSMC; fixed 2026-11-10 by removing QEMU's
# isa-applesmc, so FakeSMC is the only SMC. docs/decisions/0002-dev-vm.md.)
set -eu
. "$(dirname "$0")/lib.sh"

status=$(ssh "$RETRO" qm status "$VMID")
[ "$status" = "status: stopped" ] && { echo "VM $VMID already stopped"; exit 0; }
if gssh 'sudo -n shutdown -h now' >/dev/null 2>&1; then
    i=0
    while [ "$i" -lt 20 ]; do
        sleep 3
        [ "$(ssh "$RETRO" qm status "$VMID")" = "status: stopped" ] && { echo "VM $VMID shut down"; exit 0; }
        i=$((i + 1))
    done
    echo "VM $VMID still running after 60 s; powering off" >&2
else
    echo "no ssh to VM $VMID; powering off" >&2
fi
ssh "$RETRO" qm stop "$VMID"
echo "VM $VMID stopped"
