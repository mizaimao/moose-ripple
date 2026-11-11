#!/bin/sh
# Is Quartz Extreme on in a VM? Runs tools/qe-check in the guest (QE per display and what
# WindowServer's decision reads), prints WindowServer's GLCompositor log lines from its current
# run, and can open Exposé (F9) and Dashboard (F12) and save a picture of each.
#   VMID=210 scripts/qe-check.sh                  report only
#   VMID=210 scripts/qe-check.sh -x [OUTDIR]      also capture Exposé and Dashboard into OUTDIR
#                                                 (default /tmp): qe-expose.png, qe-dashboard.png
# CAPTURE=vnc (default) reads the VM's display through VNC (scripts/screen-vnc.sh; emulated
# displays such as virtio-gpu). CAPTURE=guest uses screencapture in the guest (scripts/screen.sh
# -g; needed for a passed-through GPU). Build the tool first: scripts/build.sh qecheck.
# See docs/re/qe-verification.md.
set -eu
. "$(dirname "$0")/lib.sh"

shots=0
[ "${1:-}" = -x ] && { shots=1; shift; }
outdir=${1:-/tmp}
bin=$ROOT/build/qe-check
[ -f "$bin" ] || die "no $bin: run scripts/build.sh qecheck"

gssh 'cat > /tmp/ripple-qe-check && chmod +x /tmp/ripple-qe-check' < "$bin"
status=0
gssh '/tmp/ripple-qe-check; s=$?; rm -f /tmp/ripple-qe-check
      pid=$(ps -axco pid,command | awk "\$2 == \"WindowServer\" {print \$1}")
      echo "WindowServer pid $pid, its GLCompositor log lines:"
      grep -h "\[$pid\] GLCompositor" /var/log/windowserver.log || echo "  none (no GL compositor)"
      exit $s' || status=$?

capture() {
    if [ "${CAPTURE:-vnc}" = guest ]; then
        "$ROOT/scripts/screen.sh" -g "$1" >/dev/null
    else
        "$ROOT/scripts/screen-vnc.sh" "$1" >/dev/null
    fi
    echo "saved $1"
}

if [ "$shots" = 1 ]; then
    ssh "$RETRO" "qm sendkey $VMID f9"; sleep 2
    capture "$outdir/qe-expose.png"
    ssh "$RETRO" "qm sendkey $VMID f9"; sleep 1
    ssh "$RETRO" "qm sendkey $VMID f12"; sleep 2
    capture "$outdir/qe-dashboard.png"
    ssh "$RETRO" "qm sendkey $VMID f12"; sleep 1
fi
exit $status
