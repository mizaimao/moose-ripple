#!/bin/sh
# Show or set the dev VM's boot flags (OpenCore keeps them in NVRAM; decision 0007).
#   scripts/bootargs.sh                       show
#   scripts/bootargs.sh '-v npci=0x2000 x=1'  set (takes effect at the next boot)
# npci=0x2000 must stay: without it 10.6 moves the virtio-gpu BAR above 4 GB (known-issues.md).
set -eu
. "$(dirname "$0")/lib.sh"

if [ $# -eq 0 ]; then
    gssh 'nvram boot-args'
    exit 0
fi
case " $* " in
    *" npci=0x2000 "*) ;;
    *) die "keep npci=0x2000 in the boot flags (docs/known-issues.md)" ;;
esac
gssh "sudo -n nvram boot-args='$*' && nvram boot-args"
