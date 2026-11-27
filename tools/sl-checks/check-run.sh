#!/bin/sh
# check-run.sh start|end LABEL: the development checks around a test or benchmark run in a
# Snow Leopard guest (2026-11-27: a run fails on its own instead of anyone looking at pictures).
#   start  notes each GL process's skip count so far and the kext's error lines so far
#   end    prints one line: what the driver left out during the run (the "skips:" counts the GL
#          bundle writes to /Library/Logs/RippleGL/PROGRAM-PID.log, see ripple_gl_skip), and the
#          kext's errors in the kernel log; exit status 1 when either is above zero
# Pair it with the host's own check (host-errors.sh) for what the host's renderer refused.
# The counts exist only where the debug log does (/Library/Logs/RippleGL), as on test guests.
set -u
what=${1:?usage: check-run.sh start|end LABEL}
label=${2:?usage: check-run.sh start|end LABEL}
d=/tmp/ripple-checks
L=/Library/Logs/RippleGL
mkdir -p $d

# The last skip count of every log: "FILE COUNT" lines.
counts() {
    for f in $L/*.log; do
        [ -f "$f" ] || continue
        n=$(grep "^skips: " "$f" 2>/dev/null | tail -1 | awk '{print $2}')
        echo "$f ${n:-0}"
    done
}
kext_errors() {
    sudo -n dmesg 2>/dev/null | grep -i "ripple" | grep -i -c "fail\|timed out\|retired\|refused\|error"
}

case $what in
start)
    counts > $d/$label.skips
    kext_errors > $d/$label.kext
    touch $d/$label.start
    ;;
end)
    [ -f $d/$label.start ] || { echo "check-run: no start for $label" >&2; exit 2; }
    counts > $d/$label.skips-end
    # Per log: the count now minus the count at the start (0 for a log that is new).
    skips=$(awk 'NR == FNR { start[$1] = $2; next } { n = $2 - ($1 in start ? start[$1] : 0); if (n > 0) { total += n; printf "%s %d\n", $1, n > "/dev/stderr" } } END { print total + 0 }' \
        $d/$label.skips $d/$label.skips-end 2> $d/$label.skips-by-log)
    kext=$(( $(kext_errors) - $(cat $d/$label.kext) ))
    [ $kext -lt 0 ] && kext=0                    # the kernel log wrapped
    verdict=PASS
    [ "$skips" -gt 0 ] || [ "$kext" -gt 0 ] && verdict=FAIL
    echo "checks $label: driver skips $skips, kext errors $kext: $verdict"
    if [ "$skips" -gt 0 ]; then
        while read -r f n; do
            echo "  $n in $(basename "$f"): $(grep "^skips: " "$f" | tail -1 | cut -c1-300)"
        done < $d/$label.skips-by-log
    fi
    [ $verdict = PASS ]
    ;;
*)
    echo "usage: check-run.sh start|end LABEL" >&2
    exit 2
    ;;
esac
