#!/bin/bash
# Run on the VM host next to a guest started by gvm.sh or refvm.sh (its NAME-qmp.sock).
# MIT licence (LICENSE at the repository root).
# portsample.sh NAME SECONDS: for SECONDS, samples of every guest processor's RDX, instruction
# pointer and privilege level ("RDX=... RIP=... CPL=...") through QEMU's monitor, about 70 of
# each processor a second. At a port read or write (in, out) RDX holds the port number, so the
# samples that sit at such an instruction say which port the guest is waiting for.
set -eu
cd "$(dirname "$0")"
end=$(( $(date +%s) + $2 ))
{
    echo '{"execute":"qmp_capabilities"}'
    while [ "$(date +%s)" -lt "$end" ]; do
        echo '{"execute":"human-monitor-command","arguments":{"command-line":"info registers -a"}}'
        sleep 0.012
    done
    sleep 1
} | socat - "UNIX-CONNECT:$PWD/$1-qmp.sock" | sed 's/\\r\\n/\n/g' |
    grep -o '[RE]DX=[0-9a-f]*\|[RE]IP=[0-9a-f]*\|CPL=[0-3]' | paste -d' ' - - -
