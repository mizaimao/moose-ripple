#!/bin/sh
# How evenly frames arrive while a desktop scenario runs on the dev VM: stutter as a number.
# The frame meter (frame-meter-run.sh) gives rates; this gives the gaps between frames and the
# delay from a key press to the screen. Two views of the same run:
#   viewer  every screen update QEMU's VNC server delivers to a client on the VM host itself
#           (tools/vncgrab/vncrec.c, timing only, Raw encoding: the best a viewer can get),
#           against the wall-clock time of each key press or click
#   guest   every screen update our GL bundle sends to the kernel (DISPLAY), by dtrace on
#           WindowServer: the gaps between them and the time each takes. Skipped when the
#           bundle isn't loaded there (Apple's software renderer) or with PACE_GUEST=0.
#   scripts/pace-run.sh SCENARIO ...     dashboard | expose | menu | drag | scroll | resize | gears
# A frame counts when it changes at least a set share of the screen (per scenario), so the
# clock and a blinking cursor don't. Gaps are taken inside a burst (frames less than 250 ms
# apart). Other VNC viewers slow the delivery: close consoles first (the count is printed).
# Scene: one Finder window with bounds {200, 100, 1000, 600}. drag takes it by its title bar,
# down-left and back, with a pointer move every 10 ms or so (as a mouse sends them). resize
# drags its bottom right corner in and out the same way. scroll first shows a long
# folder (/System/Library/Extensions) as a list in it, then turns the wheel 120 notches down and 120 up, a notch every 10 ms or so.
# PACE_ENCODING=tight asks the VNC server for Tight encoding, as a web console (noVNC) does,
# instead of Raw: what the server delivers when it has to compress, and the megabytes a second
# that takes on the wire.
# The raw times of the last run stay in tmp-ci/pace/SCENARIO.txt.
set -eu
. "$(dirname "$0")/lib.sh"

DEV=${DEV:-dev.lan}
REPO=Projects/moose-ripple
LEAD=3                              # seconds of still screen before the first input
[ $# -gt 0 ] || set -- dashboard expose menu drag gears
mkdir -p "$ROOT/tmp-ci/pace"

tag=ripple-pace-$$
rsync -a "$ROOT/tools/vncgrab/vncrec.c" "$DEV:$REPO/tools/vncgrab/"
ssh $SSH_OPTS "$DEV" "cd ~/$REPO && mkdir -p tmp-ci/rec && cc -O2 -static -o tmp-ci/rec/vncrec tools/vncgrab/vncrec.c && cat tmp-ci/rec/vncrec" |
    ssh "$RETRO" "cat > /tmp/$tag && chmod 700 /tmp/$tag"
trap 'ssh "$RETRO" "rm -f /tmp/$tag"' EXIT

gssh 'cat > /tmp/ripple-pace.d' <<'EOF'
int n;
pid$target::ripple_cgl_device_display:entry { self->d = 1; }
pid$target::IOConnectCallScalarMethod:entry /self->d/ { self->t = timestamp; }
pid$target::IOConnectCallScalarMethod:return /self->t/
{
    printf("%d %d\n", self->t / 1000, (timestamp - self->t) / 1000);
    self->t = 0;
}
pid$target::ripple_cgl_device_display:return { self->d = 0; }
tick-1s /++n >= $1/ { exit(0); }
EOF

for s in "$@"; do
    # acts: as perf-run.sh. secs: their length. minl: pixels a frame must change to count as
    # the answer to an input. minf: pixels a frame must change to count as an animation frame.
    case "$s" in
        dashboard) acts='key f12;sleep 1.5;key f12;sleep 1.5;key f12;sleep 1.5;key f12;sleep 1.5'; secs=6; minl=200000; minf=50000 ;;
        expose)    acts='key f9;sleep 1.5;key f9;sleep 1.5;key f9;sleep 1.5;key f9;sleep 1.5'; secs=6; minl=200000; minf=50000 ;;
        menu)      acts='click 3300 300;sleep 0.6;move 4400 300;sleep 0.6;move 5600 300;sleep 0.6;move 6600 300;sleep 0.6;move 7700 300;sleep 0.6;move 6600 300;sleep 0.6;move 5600 300;sleep 0.6;move 4400 300;sleep 0.6;move 3300 300;sleep 0.6;key esc;sleep 0.6'
                   secs=6; minl=8000; minf=8000 ;;
        drag)      acts='drag 15359 3563 9000 14000 150 0.008;sleep 0.5'; secs=5; minl=0; minf=50000 ;;
        scroll)    acts='move 15359 14000;sleep 0.3;wheel down 120 0.004;sleep 0.4;wheel up 120 0.004;sleep 0.5'; secs=5; minl=0; minf=50000
                   gssh "osascript -e 'tell application \"Finder\" to set target of front window to (POSIX file \"/System/Library/Extensions\" as alias)' -e 'tell application \"Finder\" to set current view of front window to list view' >/dev/null 2>&1" || true ;;
        resize)    acts='drag 25394 24247 19000 17000 100 0.008;sleep 0.5'; secs=4; minl=0; minf=50000 ;;
        gears)     acts='sleep 6'; secs=6; minl=0; minf=10000 ;;
        *)         die "unknown scenario $s" ;;
    esac
    total=$((LEAD + secs + 1))
    raw="$ROOT/tmp-ci/pace/$s.txt"

    guest=${PACE_GUEST:-1}
    if [ "$guest" = 1 ]; then
        gssh "ws=\$(ps -axo pid,comm | awk '/WindowServer\$/{print \$1}')
              ioreg -l -c RippleAccelerator | grep -q 'RippleQE. = .on' || exit 3
              (sudo -n dtrace -q -s /tmp/ripple-pace.d -p \$ws $((total + 3)) > /tmp/ripple-pace.out 2>&1 &)" || guest=0
    fi
    if [ "$s" = gears ]; then
        gssh 'cat > /tmp/ripple-gl-gears && chmod +x /tmp/ripple-gl-gears' < "$ROOT/build/gl-gears"
        # The ssh session stays open while it runs: an app left behind by a closed session
        # loses its window-server connection.
        gssh "RIPPLE_GL=1 /tmp/ripple-gl-gears $((total + 4)) > /tmp/ripple-gl-gears.out 2>&1" &
        gears_pid=$!
    fi
    sleep 3                         # dtrace attaches; gl-gears opens its window

    pw=$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 8)
    ssh "$RETRO" "VMID=$VMID PW=$pw SECS=$total LEAD=$LEAD TAG=$tag ENC=${PACE_ENCODING:-raw} ACTS='$acts' bash -s" > "$raw" <<'EOF'
set -eu
qmp1() { printf '%s\n' '{"execute":"qmp_capabilities"}' "$1" | socat -t 2 - UNIX-CONNECT:/var/run/qemu-server/$VMID.qmp; }
echo "C $(qmp1 '{"execute":"query-vnc"}' | grep -o '"websocket"' | wc -l)"
qmp1 "{\"execute\":\"set_password\",\"arguments\":{\"protocol\":\"vnc\",\"password\":\"$PW\"}}" >/dev/null
qmp1 '{"execute":"expire_password","arguments":{"protocol":"vnc","time":"+120"}}' >/dev/null
mkdir /tmp/$TAG.d
VNCREC_TIMES=1 VNCREC_ENCODING=$ENC VNC_SOCKET=/var/run/qemu-server/$VMID.vnc VNC_PASSWORD=$PW /tmp/$TAG $SECS /tmp/$TAG.d 2>/tmp/$TAG.d/err &
rec=$!
sleep $LEAD

coproc Q { socat - UNIX-CONNECT:/var/run/qemu-server/$VMID.qmp; }
read -r _ <&"${Q[0]}"
qmp() { echo "$1" >&"${Q[1]}"; local l; while read -r l <&"${Q[0]}"; do case "$l" in *'"return"'*|*'"error"'*) return 0;; esac; done; }
ev() { qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[$1]}}"; }
abs() { echo "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$1}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$2}}"; }
btn() { ev "{\"type\":\"btn\",\"data\":{\"down\":$1,\"button\":\"left\"}}"; }
mark() { echo "K $EPOCHREALTIME $1"; }
qmp '{"execute":"qmp_capabilities"}'
IFS=';' read -ra steps <<< "$ACTS"
for a in "${steps[@]}"; do
    read -r c x y x2 y2 n step <<< "$a"
    case "$c" in
        key)   mark key; qmp "{\"execute\":\"send-key\",\"arguments\":{\"keys\":[{\"type\":\"qcode\",\"data\":\"$x\"}]}}" ;;
        move)  mark move; ev "$(abs $x $y)" ;;
        click) ev "$(abs $x $y)"; sleep 0.1; mark click; btn true; sleep 0.08; btn false ;;
        drag)  ev "$(abs $x $y)"; sleep 0.2; btn true; sleep 0.1
               for i in $(seq 1 $n) $(seq $n -1 0); do
                   ev "$(abs $(( x + (x2 - x) * i / n )) $(( y + (y2 - y) * i / n )))"; sleep ${step:-0.1}
               done
               btn false ;;
        wheel) for i in $(seq 1 $y); do
                   ev "{\"type\":\"btn\",\"data\":{\"down\":true,\"button\":\"wheel-$x\"}}"
                   ev "{\"type\":\"btn\",\"data\":{\"down\":false,\"button\":\"wheel-$x\"}}"; sleep ${x2:-0.03}
               done ;;
        sleep) sleep "$x" ;;
    esac
done
kill "$Q_PID" 2>/dev/null || true
wait $rec || true
qmp1 '{"execute":"expire_password","arguments":{"protocol":"vnc","time":"now"}}' >/dev/null
sed -e 's/^/F /' /tmp/$TAG.d/times.txt
sed -e 's/^/E /' /tmp/$TAG.d/err
rm -f /tmp/$TAG.d/times.txt /tmp/$TAG.d/size.txt /tmp/$TAG.d/err
rmdir /tmp/$TAG.d
EOF

    awk -v s="$s" -v minl="$minl" -v minf="$minf" -v enc="${PACE_ENCODING:-raw}" '
        function sortarr(a, n,   i, j, t) { for (i = 2; i <= n; i++) { t = a[i]; for (j = i - 1; j >= 1 && a[j] > t; j--) a[j + 1] = a[j]; a[j + 1] = t } }
        function pct(a, n, p,   i) { if (!n) return 0; i = int(n * p + 0.999999); if (i < 1) i = 1; if (i > n) i = n; return a[i] }
        $1 == "C" { others = $2 }
        $1 == "K" { kt[++nk] = $2 }
        $1 == "F" { ft[++nf] = $2; fa[nf] = $4; fb[nf] = $5 }
        END {
            # The first update is the whole screen sent at connection: not a frame.
            lat = ""
            for (k = 1; k <= nk && minl > 0; k++) {
                d = -1
                for (i = 2; i <= nf; i++)
                    if (ft[i] > kt[k] && fa[i] >= minl) { d = (ft[i] - kt[k]) * 1000; break }
                lat = lat (d < 0 || d > 1200 ? " none" : sprintf(" %.0f", d))
            }
            prev = 0; frames = 0; ng = 0; sum = 0; over = 0
            for (i = 2; i <= nf; i++) {
                if (fa[i] < minf) continue
                frames++
                if (prev && ft[i] - prev < 0.25) { g[++ng] = (ft[i] - prev) * 1000; sum += g[ng]; bytes += fb[i]; if (g[ng] > 50) over++ }
                prev = ft[i]
            }
            sortarr(g, ng)
            printf "%s, viewer (%d other VNC viewers):", s, others
            if (lat != "") printf " input to screen%s ms;", lat
            if (ng) printf " %d frames, %.1f frames/s in bursts; gaps median %.0f ms, p95 %.0f, max %.0f; %.0f%% over 50 ms; %.1f MB/s (%s)\n", frames, 1000 * ng / sum, pct(g, ng, 0.5), pct(g, ng, 0.95), g[ng], 100 * over / ng, bytes / 1048576 / (sum / 1000), enc
            else printf " %d frames, no bursts\n", frames
        }' "$raw"
    grep '^E ' "$raw" | grep -v 'frames of' | sed -e 's/^E /  recorder: /' || true

    if [ "$guest" = 1 ]; then
        sleep 4                     # dtrace runs 3 s longer than the recording
        # dtrace prints each CPU's buffer in turn: sort by time.
        gssh 'cat /tmp/ripple-pace.out; rm -f /tmp/ripple-pace.out' | grep . | sort -n | sed -e 's/^/G /' >> "$raw"
        awk -v s="$s" '
            function sortarr(a, n,   i, j, t) { for (i = 2; i <= n; i++) { t = a[i]; for (j = i - 1; j >= 1 && a[j] > t; j--) a[j + 1] = a[j]; a[j + 1] = t } }
            function pct(a, n, p,   i) { if (!n) return 0; i = int(n * p + 0.999999); if (i < 1) i = 1; if (i > n) i = n; return a[i] }
            $1 == "G" && NF == 3 && $2 ~ /^[0-9]+$/ {
                n++; t = $2 / 1000; d[n] = $3 / 1000; dsum += d[n]
                if (prev && t - prev < 250) { g[++ng] = t - prev; gsum += g[ng]; if (g[ng] > 33.4) over++ }
                prev = t
            }
            $1 == "G" && (NF != 3 || $2 !~ /^[0-9]+$/) { print "  dtrace: " substr($0, 3) }
            END {
                if (!n) { printf "%s, guest: no screen updates seen\n", s; exit }
                sortarr(g, ng); sortarr(d, n)
                printf "%s, guest: %d screen updates", s, n
                if (ng) printf ", %.1f per s in bursts; gaps median %.1f ms, p95 %.1f, max %.1f; %.0f%% over 33 ms", 1000 * ng / gsum, pct(g, ng, 0.5), pct(g, ng, 0.95), g[ng], 100 * over / ng
                printf "; each takes median %.1f ms, p95 %.1f, max %.1f (%.2f s in all)\n", pct(d, n, 0.5), pct(d, n, 0.95), d[n], dsum / 1000
            }' "$raw"
    fi
    if [ "$s" = gears ]; then
        wait $gears_pid || true
        gssh 'grep fps /tmp/ripple-gl-gears.out | tail -2; rm -f /tmp/ripple-gl-gears.out /tmp/ripple-gl-gears' | sed -e 's/^/  gl-gears: /'
    fi
done
gssh 'rm -f /tmp/ripple-pace.d'
