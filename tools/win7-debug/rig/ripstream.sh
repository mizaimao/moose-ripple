#!/bin/bash
# Run on the VM host next to a guest started by gvm.sh or refvm.sh (its NAME-qmp.sock).
# MIT licence (LICENSE at the repository root).
# ripstream.sh NAME SECONDS: for SECONDS, samples of every guest processor's instruction
# pointer, privilege level and page table base through QEMU's monitor, one line each
# ("RIP=... CPL=... CR3=...", or EIP= for a processor in 32-bit code), on standard output; about 70 samples of each processor a second.
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
    grep -o '[RE]IP=[0-9a-f]*\|CPL=[0-3]\|CR3=[0-9a-f]*' | paste -d' ' - - -
