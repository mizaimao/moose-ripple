#!/bin/sh
# Run on the VM host, as root; tracing is switched off again at the end. MIT licence (LICENSE
# at the repository root).
# kvmexits.sh SECONDS [LABEL]: why the guest's processors left guest mode over SECONDS, counted
# by reason (the kernel's kvm_exit event), a second. EPT_MISCONFIG is a write to a device
# register that QEMU has to handle, such as a virtio notification without ioeventfd.
set -eu
T=/sys/kernel/tracing
secs=$1; label=${2:-}
out=$(mktemp /tmp/kvmexits.XXXXXX)
old=$(cat $T/buffer_size_kb | sed 's/ .*//')
echo 0 >$T/tracing_on; echo >$T/trace
echo 16384 >$T/buffer_size_kb
echo 1 >$T/events/kvm/kvm_exit/enable
echo 1 >$T/tracing_on; sleep "$secs"; echo 0 >$T/tracing_on
echo 0 >$T/events/kvm/kvm_exit/enable
cat $T/trace >"$out"; echo >$T/trace
echo "$old" >$T/buffer_size_kb
printf '%s: guest exits a second: ' "$label"
grep -o 'reason [A-Z_a-z0-9]*' "$out" | sort | uniq -c | sort -rn | head -8 |
    awk -v s="$secs" '{printf "%s %.0f, ", $3, $1 / s} END {print ""}'
rm -f "$out"
