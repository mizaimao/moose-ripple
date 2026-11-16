#!/bin/bash
# Run on the VM host, as root. MIT licence (LICENSE at the repository root).
# modbase.sh VMID [ADDRESS]: the load address of the guest module at or below ADDRESS (hex), by
# looking for its MZ header page by page; without ADDRESS, the kernel's (ntoskrnl), found from
# the first handler of the interrupt table. Read through QEMU's monitor (no debug stub needed).
set -eu
start=${2:-}
coproc Q { socat - UNIX-CONNECT:/var/run/qemu-server/$1.qmp; }
read -r _ <&"${Q[0]}"
hmp() {
    echo "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"$1\"}}" >&"${Q[1]}"
    local l
    while read -r l <&"${Q[0]}"; do
        case "$l" in *'"return"'*) printf '%s\n' "$l" > /tmp/rb-hmp.$$; return 0;; *'"error"'*) echo "$l" >&2; return 1;; esac
    done
}
echo '{"execute":"qmp_capabilities"}' >&"${Q[1]}"; read -r _ <&"${Q[0]}"
hmp "info registers"
idt=$(grep -o 'IDT= *[0-9a-f]*' /tmp/rb-hmp.$$ | head -1 | awk '{print $2}')
hmp "x /2gx 0x$idt"
set -- $(sed 's/\\r\\n/\n/g' /tmp/rb-hmp.$$ | grep -o ': 0x[0-9a-f]* 0x[0-9a-f]*' | head -1 | sed 's/: //')
lo=$(( $1 )); hi=$(( $2 ))
handler=$(( ${start:-0} ))
a=$(( handler & ~0xfff )); [ -n "$start" ] || a=$(( (lo & 0xffff) | ((lo >> 32) & 0xffff0000) | ((hi & 0xffffffff) << 32) )); a=$(( a & ~0xfff ))
i=0
while [ $i -lt 4096 ]; do
    hmp "$(printf 'x /1hx 0x%x' $a)"
    if grep -q '0x5a4d' /tmp/rb-hmp.$$; then printf 'module at 0x%x\n' $a; break; fi
    a=$(( a - 0x1000 )); i=$((i + 1))
done
rm -f /tmp/rb-hmp.$$
