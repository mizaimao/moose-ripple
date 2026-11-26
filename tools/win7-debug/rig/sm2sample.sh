#!/bin/bash
# sm2sample.sh NAME: follows guest NAME's unattended 3DMark06 run and samples its processors for 60
# seconds in each of the two SM2.0 tests: 30 s after the first full-screen mark, and 20 s after
# the second one (the tests show as two full-screen marks when loading between them takes a
# few seconds) or, when no second mark has come, 125 s after the first.
cd "$(dirname "$0")" || exit 1
name=$1
marks() { tr -d '\000\r' <"$name-com1.log" | grep -a -c "$1"; }
alive() { kill -0 "$(cat "$name.pid")" 2>/dev/null; }
until [ "$(marks 'fullscreen-1280x1024')" -ge 1 ]; do alive || exit 0; sleep 1; done
t0=$(date +%s)
sleep 30
bash ripstream.sh "$name" 60 >"$name-rip-gt1.txt" 2>"$name-rip.err" </dev/null
until [ "$(marks 'fullscreen-1280x1024')" -ge 2 ] || [ $(( $(date +%s) - t0 )) -ge 105 ]; do alive || exit 0; sleep 1; done
sleep 20
bash ripstream.sh "$name" 60 >"$name-rip-gt2.txt" 2>>"$name-rip.err" </dev/null
echo "sampled $(wc -l <"$name-rip-gt1.txt") and $(wc -l <"$name-rip-gt2.txt") lines; first mark at $(date -d @$t0 +%T), second sample from $(date -d @$(( $(date +%s) - 60 )) +%T)" >"$name-rip.log"
