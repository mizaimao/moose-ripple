#!/bin/sh
# Run on the VM host, as root, next to maintrace.sh and kvmexits.sh, after refvm.sh has started
# guest NAME. MIT licence (LICENSE at the repository root).
# prof.sh NAME: while the guest runs, over and over: ten seconds of maintrace.sh on QEMU's main
# thread, then two seconds of kvmexits.sh, each line stamped with the host's time. Results in
# NAME-prof.log; set them against the run's marks on the serial port and against busylog.sh's
# GPU log afterwards (the parts of a benchmark run do not start at fixed times).
cd "$(dirname "$0")" || exit 1
name=$1
p=$(cat "$name.pid") || exit 1
while kill -0 "$p" 2>/dev/null; do
    sh maintrace.sh "$p" 10 "$(date +%T)"
    sh kvmexits.sh 2 "$(date +%T)"
done >>"$name-prof.log" 2>&1
