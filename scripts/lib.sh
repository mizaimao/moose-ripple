# Shared settings for the moose-ripple scripts. Sourced, not run.
# Override any of these in the environment, e.g. VMID=211 scripts/screen.sh

RETRO=${RETRO:-retro}            # ssh alias of the Proxmox host (the retro rig)
VMID=${VMID:-210}                # dev VM: copy of the Snow Leopard disk, no GPU passthrough
GUEST=${GUEST:-}                 # guest address for ssh; empty until Remote Login is set up
GUEST_USER=${GUEST_USER:-}

die() { echo "$(basename "$0"): $*" >&2; exit 1; }

# Run one HMP (QEMU monitor) command on the dev VM and print its output.
hmp() {
    printf '%s\n' "$*" | ssh "$RETRO" qm monitor "$VMID"
}
