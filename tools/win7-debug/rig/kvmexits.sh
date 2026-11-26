#!/bin/sh
# Run on the VM host, as root; tracing is switched off again at the end. MIT licence (LICENSE
# at the repository root).
# kvmexits.sh SECONDS [LABEL]: why the guest's processors left guest mode over SECONDS, counted
# by reason (the kernel's kvm_exit event), a second. EPT_MISCONFIG is an access to a device
# register that the host has to handle, such as a virtio notification without ioeventfd; for
# those it also names the registers' addresses (kvm_mmio) and the guest instruction addresses
# they came from, most frequent first.
set -eu
T=/sys/kernel/tracing
secs=$1; label=${2:-}
out=$(mktemp /tmp/kvmexits.XXXXXX)
old=$(cat $T/buffer_size_kb | sed 's/ .*//')
on=$(cat $T/tracing_on)
echo 0 >$T/tracing_on; echo >$T/trace
echo 16384 >$T/buffer_size_kb
echo 1 >$T/events/kvm/kvm_exit/enable
echo 1 >$T/events/kvm/kvm_mmio/enable
echo 1 >$T/tracing_on; sleep "$secs"; echo 0 >$T/tracing_on
echo 0 >$T/events/kvm/kvm_exit/enable
echo 0 >$T/events/kvm/kvm_mmio/enable
cat $T/trace >"$out"; echo >$T/trace
echo "$old" >$T/buffer_size_kb
echo "$on" >$T/tracing_on
printf '%s: guest exits a second: ' "$label"
grep -o 'kvm_exit: .*reason [A-Z_a-z0-9]*' "$out" | grep -o 'reason [A-Z_a-z0-9]*' | sort | uniq -c | sort -rn | head -8 |
    awk -v s="$secs" '{printf "%s %.0f, ", $3, $1 / s} END {print ""}'
n=$(grep -c 'kvm_mmio:' "$out" || true)
if [ "$n" -gt $((secs * 5000)) ]; then
    printf '%s: device registers a second: ' "$label"
    grep -o 'kvm_mmio: mmio [a-z-]* len [0-9]* gpa 0x[0-9a-f]*' "$out" | awk '{print $3, $7}' | sort | uniq -c | sort -rn | head -5 |
        awk -v s="$secs" '{printf "%s %s %.0f, ", $2, $3, $1 / s} END {print ""}'
    printf '%s: from guest addresses: ' "$label"
    grep 'kvm_exit:.*reason EPT_MISCONFIG' "$out" | grep -o 'rip 0x[0-9a-f]*' | sort | uniq -c | sort -rn | head -4 |
        awk -v s="$secs" '{printf "%s %.0f, ", $3, $1 / s} END {print ""}'
fi
rm -f "$out"
