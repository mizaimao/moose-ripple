#!/bin/sh
# A Windows 9x test VM under plain QEMU on the build host, headless (no window anywhere).
#   tools/win9x/vm.sh start [QEMU options...]   start it (KVM, Pentium III, 256 MB, IDE disk)
#   tools/win9x/vm.sh stop                      power off (hard)
#   tools/win9x/vm.sh hmp 'COMMAND'             one QEMU monitor command, prints its output
#   tools/win9x/vm.sh screen OUT.png            screenshot
#   tools/win9x/vm.sh key KEY...                key presses, QEMU names: ret esc ctrl-esc alt-f4
#   tools/win9x/vm.sh type 'text'               types text (US layout); add -n for Return after
#   tools/win9x/vm.sh mouse X Y                  moves the pointer to guest pixel X Y (PS/2 mouse)
#   tools/win9x/vm.sh click [right] [double]     clicks where the pointer is
#   tools/win9x/vm.sh status                    running or stopped
# The VM's files live in VMDIR (default: tmp-win9x-vm in the repo, never committed):
#   DISK (default w98se-work.qcow2), share/ (shown to the guest as a second disk, D:),
#   cd.iso if present (a CD-ROM, e.g. a link to the Windows install disc),
#   com1.log (the guest's COM1), debug.log (bytes the guest writes to port 0xE9), sockets.
# VGA defaults to the Cirrus card the test disk was installed with; pass e.g.
#   -vga none -device virtio-vga     to test our driver's card.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
VMDIR=${VMDIR:-$root/tmp-win9x-vm}
DISK=${DISK:-w98se-work.qcow2}
cd "$VMDIR"
# VMDIR as an absolute path without symlinks: QEMU opens share/ by it after it has moved to /,
# and running() knows this VM's QEMU by it, so it has to read the same in every call
VMDIR=$(pwd -P)

# The monitor listens on localhost only (HMP_PORT); the banner, echo and prompt lines are dropped.
HMP_PORT=${HMP_PORT:-44598}
hmp() {
    printf '%s\n' "$1" | nc -q 1 127.0.0.1 "$HMP_PORT" | tr -d '\r' |
        sed -e '1d' -e '/^(qemu) /d' -e '/^(qemu)$/d'
}
# QEMU removes qemu.pid when it exits cleanly, but a crash, a kill -9 or a host reboot leaves the
# file, and its number may since belong to another process. So the pid counts (and is signalled)
# only while its command line holds this VM's shared folder. Sets pid.
running() {
    pid=$(cat qemu.pid 2>/dev/null) || return 1
    kill -0 "$pid" 2>/dev/null && grep -qaF "fat:rw:$VMDIR/share," "/proc/$pid/cmdline" 2>/dev/null
}

cmd=${1:-}; [ $# -gt 0 ] && shift
case "$cmd" in
start)
    running && { echo "vm.sh: already running (pid $(cat qemu.pid))" >&2; exit 1; }
    rm -f qmp.sock
    mkdir -p share
    vga="-vga cirrus"
    cd=; [ ! -e cd.iso ] || cd="-drive file=cd.iso,if=ide,index=2,media=cdrom,readonly=on"
    for a in "$@"; do case "$a" in -vga|virtio-vga*|VGA|bochs-display) vga= ;; esac; done
    # shellcheck disable=SC2086
    qemu-system-i386 -accel kvm -machine pc -cpu pentium3 -m 256 -rtc base=localtime \
        -drive "file=$DISK,if=ide,index=0,media=disk" \
        -drive "file=fat:rw:$VMDIR/share,format=raw,if=ide,index=1,media=disk" $cd \
        -display none -monitor "tcp:127.0.0.1:$HMP_PORT,server,nowait" -qmp unix:qmp.sock,server,nowait \
        -serial file:com1.log -debugcon file:debug.log -net none \
        -daemonize -pidfile qemu.pid $vga "$@"
    echo "started, pid $(cat qemu.pid)"
    ;;
stop)
    running || { echo "not running"; exit 0; }
    # SIGTERM shuts QEMU down the way the monitor's quit does (disks flushed, qemu.pid removed),
    # and it needs no monitor, which may be busy with another client, or be another VM's when
    # HMP_PORT differs from the one at start. A hung main loop acts on neither: SIGKILL after 10 s.
    for sig in TERM KILL; do
        kill -s "$sig" "$pid" 2>/dev/null || true
        i=0; while [ $i -lt 20 ] && running; do sleep 0.5; i=$((i + 1)); done
        running || break
    done
    running && { echo "vm.sh: QEMU (pid $pid) did not stop" >&2; exit 1; }
    rm -f qemu.pid; echo stopped
    ;;
status) if running; then echo running; else echo stopped; fi ;;
hmp) hmp "$1" ;;
screen)
    out=${1:?usage: vm.sh screen OUT.png}
    case "$out" in /*) ;; *) out=$OLDPWD/$out ;; esac
    hmp "screendump $out -f png" >/dev/null; ls -la "$out" >/dev/null && echo "$out"
    ;;
key) for k in "$@"; do hmp "sendkey $k" >/dev/null; sleep 0.15; done ;;
mouse)
    # The pointer to guest pixel X Y: into the top left corner first, then out in small steps.
    # Steps of 4 counts, spaced out, stay under Windows' pointer acceleration (1 count = 1 pixel).
    x=${1:?usage: vm.sh mouse X Y} y=${2:?usage: vm.sh mouse X Y}
    {
        i=0; while [ $i -lt 40 ]; do echo "mouse_move -120 -120"; sleep 0.01; i=$((i + 1)); done
        while [ "$x" -gt 0 ] || [ "$y" -gt 0 ]; do
            dx=$x; [ "$dx" -le 4 ] || dx=4
            dy=$y; [ "$dy" -le 4 ] || dy=4
            echo "mouse_move $dx $dy"; sleep 0.012
            x=$((x - dx)); y=$((y - dy))
        done
    } | nc -q 1 127.0.0.1 "$HMP_PORT" >/dev/null
    ;;
click)
    # click [right] [double]; HMP's mouse_button takes a bit mask: 1 left, 2 right, 4 middle
    b=1; n=1
    for a in "$@"; do case "$a" in right) b=2 ;; double) n=2 ;; esac; done
    { while [ $n -gt 0 ]; do echo "mouse_button $b"; sleep 0.05; echo "mouse_button 0"; sleep 0.08; n=$((n - 1)); done; } |
        nc -q 1 127.0.0.1 "$HMP_PORT" >/dev/null
    ;;
type)
    ret=0; [ "${1:-}" = -n ] && { ret=1; shift; }
    { printf '%s' "$1" | fold -w1; echo; } | while IFS= read -r c; do
        [ -n "$c" ] || continue
        case "$c" in
            [a-z0-9]) k=$c ;;
            [A-Z]) k=shift-$(printf '%s' "$c" | tr A-Z a-z) ;;
            " ") k=spc ;;
            .) k=dot ;; ,) k=comma ;; -) k=minus ;; =) k=equal ;; /) k=slash ;; ';') k=semicolon ;;
            :) k=shift-semicolon ;; '\') k=backslash ;; _) k=shift-minus ;; '"') k=shift-apostrophe ;;
            "'") k=apostrophe ;; '(') k=shift-9 ;; ')') k=shift-0 ;; '*') k=shift-8 ;; '&') k=shift-7 ;;
            '%') k=shift-5 ;; '$') k=shift-4 ;; '#') k=shift-3 ;; '@') k=shift-2 ;; '!') k=shift-1 ;;
            '+') k=shift-equal ;; '>') k=shift-dot ;; '<') k=shift-comma ;; '?') k=shift-slash ;;
            '[') k=bracket_left ;; ']') k=bracket_right ;; '~') k=shift-grave_accent ;;
            *) echo "vm.sh: no key for '$c'" >&2; continue ;;
        esac
        hmp "sendkey $k" >/dev/null; sleep 0.05
    done
    [ "$ret" = 0 ] || hmp "sendkey ret" >/dev/null
    ;;
*) sed -n '2,12p' "$0"; exit 1 ;;
esac
