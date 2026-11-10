# Shared settings for the moose-ripple scripts. Sourced, not run.
# Override any of these in the environment, e.g. VMID=211 scripts/screen.sh

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RETRO=${RETRO:-retro}            # ssh alias of the Proxmox host (the retro rig)
VMID=${VMID:-210}                # dev VM: copy of the Snow Leopard disk, no GPU passthrough
GUEST=${GUEST:-}                 # guest address; looked up from the VM's MAC when empty
GUEST_USER=${GUEST_USER:-user}  # has passwordless sudo in VM 210 only
GUEST_KEY=${GUEST_KEY:-$ROOT/.secrets/guest_rsa}   # git-ignored; made by ssh-keygen

die() { echo "$(basename "$0"): $*" >&2; exit 1; }

# Run one HMP (QEMU monitor) command on the dev VM and print its output.
hmp() {
    printf '%s\n' "$*" | ssh "$RETRO" qm monitor "$VMID"
}

# Print the guest's IPv4 address: the retro rig's neighbour entry for the VM's MAC.
guest_addr() {
    if [ -n "$GUEST" ]; then echo "$GUEST"; return; fi
    ssh "$RETRO" "mac=\$(qm config $VMID | sed -n 's/^net0: [a-z0-9]*=\([^,]*\),.*/\1/p')
        ip -4 neigh | grep -i \"\$mac\" | awk '{print \$1; exit}'"
}

# ssh to the guest. 10.6 ships OpenSSH 5.2, which only knows ssh-rsa (SHA-1) signatures.
# Its host key changes whenever the VM is rebuilt, so it lives in a git-ignored file.
gssh() {
    addr=$(guest_addr)
    [ -n "$addr" ] || die "no address for VM $VMID (is it running?)"
    ssh -i "$GUEST_KEY" -o IdentitiesOnly=yes \
        -o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa \
        -o UserKnownHostsFile="$ROOT/.secrets/known_hosts" -o StrictHostKeyChecking=accept-new \
        -o ConnectTimeout=10 "$GUEST_USER@$addr" "$@"
}
