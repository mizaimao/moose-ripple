#!/bin/sh
# Run on the VM host, as root, next to busylog.sh, kvmexits.sh, maintrace.sh and portsample.sh,
# while busylog.sh NAME runs. stallwatch.sh NAME: samples of what guest and host do, first
# while the GPU works (six busy-log lines in a row above 60%: "frames"), then up to three times
# when the GPU idles while QEMU's main thread is busy (two lines in a row with the GPU under
# 5% and the main thread above 60%: "stall1" to "stall3"). Each sample: the guest's exits by
# reason for four seconds, the main thread's system calls for four, the guest processors'
# instruction addresses for four. Results in results/NAME-{kvm,main,port}-TAG.txt.
# MIT licence (LICENSE at the repository root).
cd "$(dirname "$0")" || exit 1
name=$1
p=$(cat "$name.pid") || exit 1
mkdir -p results
sample() {
    echo "$(date +%T) sample $1" >>"results/$name-watch.log"
    sh kvmexits.sh 4 "$1" >"results/$name-kvm-$1.txt" 2>&1
    sh maintrace.sh "$p" 4 "$1" >"results/$name-main-$1.txt" 2>&1
    bash portsample.sh "$name" 4 >"results/$name-port-$1.txt" 2>&1
}
hi=0
while kill -0 "$p" 2>/dev/null && [ $hi -lt 6 ]; do
    sleep 5
    g=$(tail -1 "$name-busy.log" | awk '{print $3+0}')
    if [ "${g:-0}" -ge 60 ]; then hi=$((hi+1)); else hi=0; fi
done
kill -0 "$p" 2>/dev/null && sample frames
k=1; lo=0
while kill -0 "$p" 2>/dev/null && [ $k -le 3 ]; do
    sleep 5
    l=$(tail -1 "$name-busy.log")
    g=$(echo "$l" | awk '{print $3+0}')
    m=$(echo "$l" | tr ' ' '\n' | grep "^$p=" | cut -d= -f2 | cut -d. -f1)
    if [ "${g:-99}" -lt 5 ] && [ "${m:-0}" -ge 60 ]; then lo=$((lo+1)); else lo=0; fi
    if [ $lo -ge 2 ]; then sample "stall$k"; k=$((k+1)); lo=0; fi
done
echo "$(date +%T) watch over" >>"results/$name-watch.log"
