#!/bin/sh
# Run on the VM host, as root. MIT licence (LICENSE at the repository root).
# gpubusy.sh SECONDS: share of time the Intel GPU was awake (not in RC6) and its clock, over SECONDS
g=/sys/class/drm/card0/gt/gt0
a=$(cat $g/rc6_residency_ms); f1=$(cat $g/rps_act_freq_mhz); sleep $1; b=$(cat $g/rc6_residency_ms); f2=$(cat $g/rps_act_freq_mhz)
echo "GPU awake $(( 100 - (b - a) / ($1 * 10) ))% of $1 s, clock $f1 -> $f2 MHz (min $(cat $g/rps_min_freq_mhz), max $(cat $g/rps_max_freq_mhz))"
