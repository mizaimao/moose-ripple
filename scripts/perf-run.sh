#!/bin/sh
# Measure where the time goes while a desktop scenario runs on the dev VM (performance stage,
# 2026-11-11). Drives the guest through QMP (keys, pointer) and samples on the rig and in the
# guest over the same interval:
#   guest   WindowServer CPU (cputime delta), our bundle's present stats (when QE is on)
#   rig     QEMU main thread CPU (virglrenderer runs there), vCPU threads CPU,
#           host GPU render-engine busy time (i915 fdinfo of QEMU's renderD128),
#           RC6 residency (GPU idle power state), mean actual GPU frequency
#   scripts/perf-run.sh SCENARIO      idle | expose | dashboard | menu | drag | gears
# Prints one line per measurement. Needs nothing installed (procfs, sysfs, socat for QMP).
set -eu
. "$(dirname "$0")/lib.sh"

scen=${1:-idle}
case "$scen" in
    idle)      acts='sleep 6' ;;
    expose)    acts='key f9;sleep 1.5;key f9;sleep 1.5;key f9;sleep 1.5;key f9;sleep 1.5' ;;
    dashboard) acts='key f12;sleep 1.5;key f12;sleep 1.5;key f12;sleep 1.5;key f12;sleep 1.5' ;;
    # File, then across Edit, View, Go, Window and back, twice; Escape.
    menu)      acts='click 3300 300;sleep 0.6;move 4400 300;sleep 0.6;move 5600 300;sleep 0.6;move 6600 300;sleep 0.6;move 7700 300;sleep 0.6;move 6600 300;sleep 0.6;move 5600 300;sleep 0.6;move 4400 300;sleep 0.6;move 3300 300;sleep 0.6;key esc' ;;
    # Finder window title bar (x 766, y 33 of 1280x800) dragged down-left and back in 40 steps.
    drag)      acts='drag 19609 1351 15000 9000 20' ;;
    gears)     acts='sleep 6' ;;
    *)         die "unknown scenario $scen" ;;
esac

if [ "$scen" = gears ]; then
    gssh 'cat > /tmp/ripple-gl-gears && chmod +x /tmp/ripple-gl-gears' < "$ROOT/build/gl-gears"
    gssh '/tmp/ripple-gl-gears 9 > /tmp/ripple-gl-gears.out 2>&1 &'
    sleep 2
fi
ws0=$(gssh 'ps -axo cputime,comm | awk "/WindowServer$/{print \$1}"')

ssh "$RETRO" "VMID=$VMID ACTS='$acts' bash -s" <<'EOF'
set -eu
pid=$(cat /var/run/qemu-server/$VMID.pid)
hz=$(getconf CLK_TCK)
ticks() { awk '{print $14 + $15}' /proc/$pid/task/$1/stat; }
vcpus() { for t in /proc/$pid/task/*; do grep -q '^CPU ' $t/comm && ticks $(basename $t); done | awk '{s+=$1} END {print s+0}'; }
gpuns() { for fd in /proc/$pid/fd/*; do case "$(readlink $fd)" in /dev/dri/renderD*) awk '/drm-engine-render/{print $2}' /proc/$pid/fdinfo/$(basename $fd); return;; esac; done; echo 0; }
rc6() { cat /sys/class/drm/card0/gt/gt0/rc6_residency_ms; }
now() { date +%s%N; }

coproc Q { socat - UNIX-CONNECT:/var/run/qemu-server/$VMID.qmp; }
read -r _ <&"${Q[0]}"
qmp() { echo "$1" >&"${Q[1]}"; local l; while read -r l <&"${Q[0]}"; do case "$l" in *'"return"'*|*'"error"'*) return 0;; esac; done; }
ev() { qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[$1]}}"; }
abs() { echo "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$1}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$2}}"; }
btn() { ev "{\"type\":\"btn\",\"data\":{\"down\":$1,\"button\":\"left\"}}"; }
qmp '{"execute":"qmp_capabilities"}'

( while :; do cat /sys/class/drm/card0/gt_act_freq_mhz; sleep 0.2; done ) > /tmp/ripple-perf-freq.$$ &
fpid=$!
t0=$(now); m0=$(ticks $pid); v0=$(vcpus); g0=$(gpuns); r0=$(rc6)
IFS=';' read -ra steps <<< "$ACTS"
for a in "${steps[@]}"; do
    read -r c x y x2 y2 n <<< "$a"
    case "$c" in
        key)   qmp "{\"execute\":\"send-key\",\"arguments\":{\"keys\":[{\"type\":\"qcode\",\"data\":\"$x\"}]}}" ;;
        move)  ev "$(abs $x $y)" ;;
        click) ev "$(abs $x $y)"; sleep 0.1; btn true; sleep 0.08; btn false ;;
        drag)  ev "$(abs $x $y)"; sleep 0.2; btn true; sleep 0.1
               for i in $(seq 1 $n) $(seq $n -1 0); do
                   ev "$(abs $(( x + (x2 - x) * i / n )) $(( y + (y2 - y) * i / n )))"; sleep 0.1
               done
               btn false ;;
        sleep) sleep "$x" ;;
    esac
done
t1=$(now); m1=$(ticks $pid); v1=$(vcpus); g1=$(gpuns); r1=$(rc6)
kill $fpid 2>/dev/null || true
kill "$Q_PID" 2>/dev/null || true
ms=$(( (t1 - t0) / 1000000 ))
awk -v ms=$ms -v hz=$hz -v m=$((m1 - m0)) -v v=$((v1 - v0)) -v g=$((g1 - g0)) -v r=$((r1 - r0)) '
    BEGIN { printf "rig: %.1f s; QEMU main thread %.0f%%, vCPUs %.0f%% (of one core); host GPU render busy %.0f%%, RC6 %.0f%%, ",
            ms / 1000, 100 * m / hz / (ms / 1000), 100 * v / hz / (ms / 1000), 100 * g / 1e6 / ms, 100 * r / ms }'
awk '{s+=$1; n++} END {printf "GPU clock %.0f MHz (mean)\n", n ? s/n : 0}' /tmp/ripple-perf-freq.$$
rm -f /tmp/ripple-perf-freq.$$
echo "elapsed_ms=$ms"
EOF

ws1=$(gssh 'ps -axo cputime,comm | awk "/WindowServer$/{print \$1}"; grep stats: "$(ls -t /Library/Logs/RippleGL/WindowServer-*.log 2>/dev/null | head -1)" 2>/dev/null | tail -2 || true; cat /tmp/ripple-gl-gears.out 2>/dev/null | tail -2; rm -f /tmp/ripple-gl-gears.out')
secs() { echo "$1" | awk -F: '{print ($1 * 60) + $2}'; }
echo "guest: WindowServer CPU $(awk -v a="$(secs "$ws0")" -v b="$(secs "$(echo "$ws1" | head -1)")" 'BEGIN {printf "%.1f s", b - a}') during the scenario"
echo "$ws1" | tail -n +2
