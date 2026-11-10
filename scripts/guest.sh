#!/bin/sh
# Run a command in the dev VM over ssh (or open a shell with no arguments).
#   scripts/guest.sh 'sudo -n kextstat | grep -v com.apple'
set -eu
. "$(dirname "$0")/lib.sh"
gssh "$@"
