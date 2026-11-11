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

# Print the guest's IPv4 address (DHCP): the retro rig's neighbour entry for the VM's MAC.
# If the entry has expired, ping the subnet once from the rig to refresh it.
guest_addr() {
    if [ -n "$GUEST" ]; then echo "$GUEST"; return; fi
    ssh -n "$RETRO" "mac=\$(qm config $VMID | sed -n 's/^net0: [a-z0-9]*=\([^,]*\),.*/\1/p')
        find() { ip -4 neigh | grep -i \"\$mac\" | grep -v FAILED | awk '{print \$1; exit}'; }
        a=\$(find)
        if [ -z \"\$a\" ]; then
            net=\$(ip -4 -o addr show vmbr0 | awk '{print \$4}' | cut -d. -f1-3)
            for i in \$(seq 1 254); do ping -c1 -W1 \$net.\$i >/dev/null 2>&1 & done; wait
            a=\$(find)
        fi
        echo \$a"
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

# A connect timeout on every ssh, scp and rsync to the build host: no script should hang on an
# unreachable host. It is also needed on the owner's laptop since 2026-11-11: Apple's ssh 10.5
# without one connects by host name through another code path, which fails there ("connect to
# host dev.lan port 22: Undefined error: 0"); with one it resolves the name itself and works.
SSH_OPTS="-o ConnectTimeout=15"
export RSYNC_RSH="ssh $SSH_OPTS"
