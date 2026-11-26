#!/bin/sh
# Run on the VM host, as root, next to busylog.sh and pioports.sh, while busylog.sh NAME runs.
# piowatch.sh NAME: the guest's port accesses, sampled for three seconds while the GPU works
# (six busy-log lines in a row above 60%: "frames") and up to twice when the GPU idles while
# QEMU's main thread is busy (two lines in a row under 5% and above 60%: "idle1", "idle2").
# Results in results/NAME-pio-TAG.txt. MIT licence (LICENSE at the repository root).
cd "$(dirname "$0")" || exit 1
name=$1
p=$(cat "$name.pid") || exit 1
mkdir -p results
tids=$(grep -o '"thread-id": [0-9]*' "$name-vcpus.txt" | awk '$2 > 0 {printf "%s ", $2}')
sample() {
    echo "$(date +%T) sample $1" >>"results/$name-watch.log"
    sh pioports.sh 3 "$1" "$tids" >"results/$name-pio-$1.txt" 2>&1
}
hi=0
while kill -0 "$p" 2>/dev/null && [ $hi -lt 6 ]; do
    sleep 5
    g=$(tail -1 "$name-busy.log" | awk '{print $3+0}')
    if [ "${g:-0}" -ge 60 ]; then hi=$((hi+1)); else hi=0; fi
done
kill -0 "$p" 2>/dev/null && sample frames
k=1; lo=0
while kill -0 "$p" 2>/dev/null && [ $k -le 2 ]; do
    sleep 5
    l=$(tail -1 "$name-busy.log")
    g=$(echo "$l" | awk '{print $3+0}')
    m=$(echo "$l" | tr ' ' '\n' | grep "^$p=" | cut -d= -f2 | cut -d. -f1)
    if [ "${g:-99}" -lt 5 ] && [ "${m:-0}" -ge 60 ]; then lo=$((lo+1)); else lo=0; fi
    if [ $lo -ge 2 ]; then sample "idle$k"; k=$((k+1)); lo=0; sleep 20; fi
done
echo "$(date +%T) watch over" >>"results/$name-watch.log"
