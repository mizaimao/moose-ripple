#!/bin/sh
# What the host does while the UTM test VM works: call-stack samples of the VM's QEMU process
# on the Mac, taken in back-to-back chunks with macOS's own `sample`, plus the display draws
# UTM's debug log recorded over the same time. tools/win7-debug/hostprof.py reads the result.
#
#   tools/win7-debug/utm-hostprof.sh SECONDS OUTDIR [INTERVAL_MS]
#
# Start it, then start the workload in the guest (for the Explorer move and resize gestures:
# `tools/win7-debug/utm.sh type -n 'C:\rb\pkg\rb.cmd q79resize hostprofile'`). Leave a few
# idle seconds at the end for a baseline. INTERVAL_MS is the time between samples (default 1);
# `sample` stops the process for every sample, so compare the workload's own update rate with
# an unprofiled run, and repeat with 5 if the two differ much.
#
# It runs as the logged-in user and never asks for more: if macOS refuses to let `sample` look
# at the process, the refusal is printed and nothing else is tried.
# MIT licence (LICENSE at the repository root).
set -eu
[ $# -ge 2 ] || { sed -n '2,16p' "$0" >&2; exit 2; }
MAC=${MAC:-macserver.lan}
VM=${VM:-ripple-win7}
QMPSOCK=${QMPSOCK:-rb-qmp-w7.sock}
secs=$1
out=$2
ival=${3:-1}
mkdir -p "$out"

# The remote part is zsh (the Mac's own shell) for its sub-second clock. Only pids, sample
# reports and "gl draw" lines leave the Mac; the debug log's other lines stay there.
ssh "$MAC" zsh -s -- "$secs" "$ival" "$QMPSOCK" "$VM" > "$out/hostprof.tar" <<'EOF'
zmodload zsh/datetime
secs=$1 ival=$2 sock=$3 vm=$4
pid=$(pgrep -f "${sock/./[.]}" | head -1)
[[ -n $pid ]] || { print -u2 "no QEMU process with $sock in its arguments: is the VM running?"; exit 1 }
d=$(mktemp -d /tmp/rb-hostprof.XXXXXX)
log="$HOME/Library/Containers/com.utmapp.UTM/Data/Documents/$vm.utm/Data/debug.log"
start=$EPOCHREALTIME
print "pid $pid start $start interval_ms $ival chunk_seconds 1" > $d/run.txt
n=0
while (( EPOCHREALTIME < start + secs )); do
    f=$d/s-${(l:4::0:)n}.txt
    t0=$EPOCHREALTIME
    if ! sample $pid 1 $ival -mayDie -file $f >/dev/null 2>$d/sample-error.txt; then
        print -u2 "sample was refused or failed; its message:"
        cat $d/sample-error.txt >&2
        rm -rf $d
        exit 1
    fi
    # the process's CPU time so far, for the cores used between two chunks
    print "$n $t0 $EPOCHREALTIME $(ps -o time= -p $pid | tr -d ' ')" >> $d/chunks.txt
    (( n++ ))
done
strftime -s day '%Y-%m-%d %H:%M:%S' ${start%.*}
grep -a 'gl draw' "$log" 2>/dev/null | grep -v -i osk | awk -v s="$day" '($1 " " $2) >= s' > $d/draws.txt
rm -f $d/sample-error.txt
tar -C $d -cf - .
rm -rf $d
EOF
tar -xf "$out/hostprof.tar" -C "$out"
rm -f "$out/hostprof.tar"
echo "$out: $(ls "$out" | grep -c '^s-') chunks, $(wc -l < "$out/draws.txt" | tr -d ' ') draws"
