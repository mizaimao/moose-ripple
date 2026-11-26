#!/bin/sh
# Run on the VM host, as root, next to rcsbusy.sh. busylog.sh NAME: while guest NAME (NAME.pid)
# runs, a line every five seconds in NAME-busy.log: the time, the Intel GPU's render-engine
# busy share (rcsbusy.sh) and QEMU's threads above 3% of a processor as thread-id=percent.
# NAME-vcpus.txt gets the thread ids of the guest's processors, to tell them from QEMU's own
# threads (its main thread draws for the guest). MIT licence (LICENSE at the repository root).
cd "$(dirname "$0")" || exit 1
name=$1
p=$(cat "$name.pid") || exit 1
(printf '%s\n' '{"execute":"qmp_capabilities"}' '{"execute":"query-cpus-fast"}'; sleep 1) |
    socat - "UNIX-CONNECT:$name-qmp.sock" | grep -o '"thread-id": [0-9]*' | tr '\n' ' ' >"$name-vcpus.txt"
echo "main thread $p" >>"$name-vcpus.txt"
while kill -0 "$p" 2>/dev/null; do
    b=$(sh rcsbusy.sh 5 | sed 's/render engine busy //; s/ of.*//')
    t=$(top -H -b -n1 -p "$p" | awk 'NR>7 && $9>3 {printf "%s=%s ", $1, $9}')
    echo "$(date +%T) gpu $b threads $t"
done >>"$name-busy.log"
