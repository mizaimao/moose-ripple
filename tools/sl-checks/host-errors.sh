#!/bin/sh
# host-errors.sh: the host side of the development checks (see check-run.sh): what QEMU's virgl
# renderer reported as an error during a run. Run on the host.
#   host-errors.sh mark [LOG]         prints a mark to pass to "count" later
#   host-errors.sh count MARK [LOG]   prints "host renderer errors N" and the first few lines;
#                                     exit status 1 when N is above zero
# Without LOG it reads the systemd journal (a Proxmox host: QEMU's output lands there); with LOG,
# that file (UTM: the VM's Data/debug.log). Lines that hold the SMC key are never printed.
set -u
what=${1:?usage: host-errors.sh mark [LOG] | count MARK [LOG]}
pattern='vrend.*(error|fail|illegal|invalid|lost)|virgl.*(error|fail)|gl error|context.*(lost|destroyed)'
case $what in
mark)
    if [ $# -ge 2 ]; then wc -l < "$2" | tr -d ' '; else date '+%Y-%m-%d %H:%M:%S'; fi
    ;;
count)
    mark=${2:?usage: host-errors.sh count MARK [LOG]}
    if [ $# -ge 3 ]; then
        lines=$(tail -n +"$((mark + 1))" "$3" | grep -v -i osk | grep -E -i "$pattern")
    else
        lines=$(journalctl --since "$mark" -o cat 2>/dev/null | grep -v -i osk | grep -E -i "$pattern")
    fi
    n=$(printf '%s' "$lines" | grep -c .)
    echo "host renderer errors $n"
    [ "$n" -gt 0 ] && printf '%s\n' "$lines" | head -5 | cut -c1-200
    [ "$n" -eq 0 ]
    ;;
*)
    echo "usage: host-errors.sh mark [LOG] | count MARK [LOG]" >&2
    exit 2
    ;;
esac
