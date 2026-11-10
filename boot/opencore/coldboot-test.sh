#!/bin/sh
# Cold-boot test: start the VM from power-off, wait for ssh, record what booted, shut down
# cleanly from inside the guest, check that the VM powers off. Repeat N times.
#   VMID=212 boot/opencore/coldboot-test.sh [N]       N defaults to 10
#   CHECK='guest command' adds that command's output to each line, e.g.
#   CHECK='ioreg -rc RippleFramebuffer -d1 | grep -c " registered"'
# One line per boot; exits non-zero if any boot or shutdown failed. Uses scripts/lib.sh.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../../scripts/lib.sh"
ROOT=$(cd "$here/../.." && pwd)          # lib.sh guesses ROOT from $0; fix it for this folder
GUEST_KEY=$ROOT/.secrets/guest_rsa

n=${1:-10}
settle=20        # seconds of idle desktop before shutting down
fail=0
[ "$(ssh "$RETRO" qm status "$VMID")" = "status: stopped" ] || die "VM $VMID is not stopped"

i=1
while [ "$i" -le "$n" ]; do
    t0=$(date +%s)
    ssh "$RETRO" qm start "$VMID"
    up=
    while [ $(($(date +%s) - t0)) -lt 180 ]; do
        sleep 3
        a=$(guest_addr)
        if [ -n "$a" ] && GUEST=$a gssh true 2>/dev/null; then up=$(($(date +%s) - t0)); break; fi
    done
    if [ -z "$up" ]; then
        echo "boot $i: FAIL no ssh after 180 s"; fail=1
        "$here/../../scripts/screen.sh" "/tmp/opencore-coldboot-fail-$i.png" >/dev/null 2>&1 || true
        ssh "$RETRO" qm stop "$VMID"
        i=$((i + 1)); continue
    fi
    sleep "$settle"
    info=$(GUEST=$a gssh 'printf "%s tsc=%s panics=%s other-kexts=%s tymce=%s" \
        "$(uname -m)" "$(sysctl -n machdep.tsc.frequency)" \
        "$(ls /Library/Logs/DiagnosticReports 2>/dev/null | grep -c "^Kernel")" \
        "$(kextstat -l | grep -vc com.apple)" "$(kextstat -l | grep -c TyMCE)"')
    [ -z "${CHECK:-}" ] || info="$info check=$(GUEST=$a gssh "$CHECK" | tr '\n' ' ')"
    t1=$(date +%s)
    GUEST=$a gssh 'sudo -n shutdown -h now' >/dev/null 2>&1 || true
    down=
    while [ $(($(date +%s) - t1)) -lt 60 ]; do
        sleep 2
        if [ "$(ssh "$RETRO" qm status "$VMID")" = "status: stopped" ]; then down=$(($(date +%s) - t1)); break; fi
    done
    if [ -z "$down" ]; then
        echo "boot $i: ssh after ${up}s, $info, FAIL still running 60 s after shutdown"; fail=1
        ssh "$RETRO" qm stop "$VMID"
    else
        echo "boot $i: ssh after ${up}s, $info, powered off ${down}s after shutdown"
    fi
    i=$((i + 1))
done
exit "$fail"
