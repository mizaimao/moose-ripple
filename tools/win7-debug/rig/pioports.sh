#!/bin/sh
# Run on the VM host, as root; tracing is switched off again at the end. MIT licence (LICENSE
# at the repository root).
# pioports.sh SECONDS [LABEL [THREAD-IDS]]: which I/O ports the guests' processors (or only the
# threads named, a guest's processor threads) read and wrote over
# SECONDS (the kernel's kvm_pio event), a second, most frequent first, and the guest
# instruction addresses the port accesses came from. A port that the host's kernel handles
# itself (the interval timer, the interrupt controllers) costs a microsecond or two; one that
# QEMU handles (the ACPI timer, the real-time clock, a serial port) several times that, and
# some of those wait for QEMU's lock.
set -eu
T=/sys/kernel/tracing
secs=$1; label=${2:-}; tids=${3:-}
out=$(mktemp /tmp/pioports.XXXXXX)
old=$(sed 's/ .*//' $T/buffer_size_kb)
on=$(cat $T/tracing_on)
echo 0 >$T/tracing_on; echo >$T/trace
echo 32768 >$T/buffer_size_kb
echo "$tids" >$T/set_event_pid
echo 1 >$T/events/kvm/kvm_pio/enable
echo 1 >$T/events/kvm/kvm_exit/enable
echo 1 >$T/tracing_on; sleep "$secs"; echo 0 >$T/tracing_on
echo 0 >$T/events/kvm/kvm_pio/enable
echo 0 >$T/events/kvm/kvm_exit/enable
cat $T/trace >"$out"; echo >$T/trace
echo >$T/set_event_pid
echo "$old" >$T/buffer_size_kb
echo "$on" >$T/tracing_on
printf '%s: port accesses a second: ' "$label"
grep -o 'kvm_pio: pio_[a-z]* at 0x[0-9a-f]*' "$out" | awk '{print $2, $4}' | sort | uniq -c | sort -rn | head -8 |
    awk -v s="$secs" '{printf "%s %s %.0f, ", $2, $3, $1 / s} END {print ""}'
printf '%s: from guest addresses: ' "$label"
grep 'kvm_exit:.*reason IO_INSTRUCTION' "$out" | grep -o 'rip 0x[0-9a-f]*' | sort | uniq -c | sort -rn | head -6 |
    awk -v s="$secs" '{printf "%s %.0f, ", $3, $1 / s} END {print ""}'
printf '%s: lost trace events: ' "$label"; grep -c 'LOST' "$out" || true
rm -f "$out"
