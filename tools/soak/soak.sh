#!/bin/sh
# A stability run inside the Mac OS X guest: test programs in turn for hours, one line per
# step and a health line per round in soak.log, next to this script.
#
#   SOAK_SECONDS=7200 nohup ./soak.sh > /dev/null 2>&1 &
#
# Start it from an ssh session of the logged-in user; it keeps running after the session ends.
# Put next to it, from the repo's build/: ci-effects, gl-gears, guest-input; and any picture as
# fish.jpg. Every round runs what it finds:
#   - the Core Image comparisons against Apple's software renderer (ci-effects),
#   - gl-gears, which waits for the refresh, in a window that reaches under the Dock,
#   - Exposé and Dashboard, opened and closed by key,
#   - OpenArena 0.8.8's timedemo, in a window under the Dock and in a smaller one
#     (OA_DIR, default ~/ripple-test/oa/openarena-0.8.8),
#   - Cinebench R11.5's OpenGL test with its reference check (CB_DIR, default ~/ripple-test/cb),
#   - Real Racing 2, started, taken into a quick race for two minutes and stopped
#     (found with Spotlight; the clicks are for a 1280x800 screen).
# The games and Cinebench are not part of this project and are not needed for the rest.
# The health line: kext errors in the kernel log, WindowServer's memory, wired and free
# memory, crash reports of the day, commands that took over 100 ms. At the end the kext's last
# messages are saved as dmesg-end.txt and the guest shuts down (SOAK_SHUTDOWN=0: it stays up).
# sudo must work without a password for dmesg and the shutdown. The driver's logs have to be
# on (/Library/Logs/RippleGL) for the Real Racing 2 frame counts.
D=$(cd "$(dirname "$0")" && pwd)
L=$D/soak.log
RL=/Library/Logs/RippleGL
OA=${OA_DIR:-$HOME/ripple-test/oa/openarena-0.8.8}
CB=${CB_DIR:-$HOME/ripple-test/cb}
secs=${SOAK_SECONDS:-7200}
cd "$D" || exit 1
t0=$(date +%s)
log() { echo "$(date '+%H:%M:%S') $*" >> "$L"; }

openarena() {   # $1: r_mode (6 = 1024x768, 4 = 800x600)
    ( sleep 300; killall openarena.ub 2>/dev/null ) &
    guard=$!
    ( cd "$OA" && "./OpenArena 0.8.8 r28.app/Contents/MacOS/openarena.ub" +set fs_basepath "$OA" \
        +set r_fullscreen 0 +set r_mode $1 +set s_initsound 0 +set com_introplayed 1 \
        +timedemo 1 +demo demo088-test1 +set nextdemo "timedemo; quit" > /tmp/soak-oa.out 2>&1 )
    kill $guard 2>/dev/null
    grep -i ' frames .* fps' /tmp/soak-oa.out | cut -c1-60
}

racing() {      # prints the result of two minutes in a quick race
    app=$(mdfind -name 'Real Racing 2.app' 2>/dev/null | head -1)
    [ -n "$app" ] || return 1
    open "$app"
    t=0; p=
    while [ $t -lt 240 ]; do   # the menu is there when a 5 s window shows 150 presents
        p=$(ps -axo pid,comm | grep "MacOS/Real Racing [2]" | awk '{print $1}')
        f=$(grep "stats: window (on the GPU" "$RL/Real Racing 2-$p.log" 2>/dev/null | tail -1 |
            sed -e 's/.*: \([0-9]*\) presents.*/\1/')
        [ -n "$f" ] && [ "$f" -ge 150 ] && break
        sleep 5; t=$((t + 5))
    done
    [ -n "$p" ] || { echo "did not start"; return 0; }
    sleep 6
    ./guest-input "click 446 170" "sleep 6" "click 640 220" > /dev/null 2>&1
    sleep 16
    g="$RL/Real Racing 2-$p.log"
    a=$(grep -c 'stats: window (on the GPU' "$g" 2>/dev/null)
    sleep 120
    echo "menu after $t s; $(grep 'stats: window (on the GPU' "$g" | tail -n +$((a + 1)) |
        sed -e 's/.*: \([0-9]*\) presents.*/\1/' |
        awk '{n++; s+=$1; if (n==1||$1<m) m=$1} END {if (n) printf "mean %.1f frames/s, lowest window %.1f, %d windows", s/n/5, m/5, n; else printf "no frame counts"}')"
    kill $p 2>/dev/null
    sleep 3
}

log "START for $secs s; kext $(md5 -q /System/Library/Extensions/RippleVirtioGPU.kext/Contents/MacOS/RippleVirtioGPU | cut -c1-8) bundle $(md5 -q /System/Library/Extensions/RippleGLDriver.bundle/Contents/MacOS/RippleGLDriver | cut -c1-8); $(nvram boot-args | cut -c11-120)"
round=0
while [ $(( $(date +%s) - t0 )) -lt $secs ]; do
    round=$((round + 1))
    if [ -d "$OA" ]; then
        log "r$round openarena 1024x768: $(openarena 6)"
        log "r$round openarena 800x600: $(openarena 4)"
    fi
    if [ -d "$CB/CINEBENCH OSX.app" ]; then
        rm -f /tmp/soak-cb.ts
        ( cd "$CB" && script -q /tmp/soak-cb.ts "./CINEBENCH OSX.app/Contents/MacOS/CINEBENCH OSX" -cb_opengl > /dev/null 2>&1 & )
        i=0
        while [ $i -lt 150 ]; do
            grep -a -q "Shading (OpenGL) Reference" /tmp/soak-cb.ts 2>/dev/null && break
            sleep 2; i=$((i + 2))
        done
        log "r$round cinebench: $(grep -a 'errorpixel\|Shading' /tmp/soak-cb.ts | tr -d '\r' | tr '\n' ';' | sed 's/  */ /g' | cut -c1-130)"
        sleep 2
        for p in $(ps axo pid,comm | grep 'MacOS/CINEBENC[H]' | awk '{print $1}'); do kill -9 $p; done
    fi
    [ -x ./guest-input ] && r=$(racing) && log "r$round real racing 2: $r"
    if [ -x ./ci-effects ] && [ -f fish.jpg ]; then
        log "r$round core image filters: $(./ci-effects filters fish.jpg /tmp/soak-x.png 2>&1 | grep -i differ | tail -1 | cut -c1-110)"
        log "r$round photo booth effects: $(./ci-effects booth fish.jpg /tmp/soak-x.png 2>&1 | grep -i differ | tail -1 | cut -c1-110)"
    fi
    [ -x ./gl-gears ] && log "r$round gl-gears 1280x720: $(./gl-gears -w 1280x720 -f 900 2>&1 | tail -1 | cut -c1-70)"
    # F9 and F12: Exposé and Dashboard, open and closed again
    [ -x ./guest-input ] && ./guest-input "key 101" "sleep 2" "key 101" "sleep 2" "key 111" "sleep 3" "key 111" "sleep 2" > /dev/null 2>&1
    ws=$(ps axo rss,comm | awk '/WindowServer$/ {printf "%d", $1/1024}')
    log "r$round health: $(( ($(date +%s) - t0) / 60 )) min; kext errors $(sudo -n dmesg | grep -i ripple | grep -i -c 'fail\|timed out\|retired'); WindowServer ${ws} MB; wired $(vm_stat | awk '/wired/ {printf "%d", $4*4096/1048576}') MB, free $(vm_stat | awk '/Pages free/ {printf "%d", $3*4096/1048576}') MB; crash reports $(ls /Library/Logs/CrashReporter ~/Library/Logs/CrashReporter 2>/dev/null | grep -c "$(date '+%Y-%m-%d')"); over 100 ms: $(ioreg -c RippleFramebuffer -l | grep RippleCommandsOver100ms | sed 's/.*= //')"
done
sudo -n dmesg | grep -i ripple | tail -40 > "$D/dmesg-end.txt"
log "DONE after $round rounds"
[ "${SOAK_SHUTDOWN:-1}" = 1 ] && sudo -n shutdown -h now
