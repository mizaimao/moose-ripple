#!/bin/bash
# Run on the VM host, as root. MIT licence (LICENSE at the repository root).
# A sampling profile of the guest: tools/win7-debug/README.md, "Where a guest spends its time".
# ripsample.sh VMID COUNT: COUNT samples of every vCPU's RIP, CPL and CR3 through QMP, one line each
set -eu
coproc Q { socat - UNIX-CONNECT:/var/run/qemu-server/$1.qmp; }
read -r _ <&"${Q[0]}"
qmp() {
    echo "$1" >&"${Q[1]}"
    local l
    while read -r l <&"${Q[0]}"; do
        case "$l" in *'"return"'*) printf '%s\n' "$l"; return 0;; *'"error"'*) echo "$l" >&2; return 0;; esac
    done
}
qmp '{"execute":"qmp_capabilities"}' >/dev/null
i=0
while [ $i -lt $2 ]; do
    qmp '{"execute":"human-monitor-command","arguments":{"command-line":"info registers -a"}}' > /tmp/rb-rip.$$
    sed 's/\\r\\n/\n/g' < /tmp/rb-rip.$$ | grep -o 'RIP=[0-9a-f]*\|CPL=[0-3]\|CR3=[0-9a-f]*' | paste -d' ' - - -
    i=$((i + 1))
done
rm -f /tmp/rb-rip.$$
