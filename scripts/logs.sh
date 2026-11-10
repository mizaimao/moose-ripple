#!/bin/sh
# Show the dev VM's recent kernel log lines.
#   scripts/logs.sh              last 40 lines of /var/log/kernel.log
#   scripts/logs.sh PATTERN      lines matching PATTERN from kernel.log and system.log
set -eu
. "$(dirname "$0")/lib.sh"

if [ $# -eq 0 ]; then
    gssh 'tail -n 40 /var/log/kernel.log'
else
    gssh "grep -h -- '$1' /var/log/kernel.log /var/log/system.log | tail -n 40"
fi
