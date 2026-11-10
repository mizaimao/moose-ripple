#!/bin/sh
# The M0 loop in one command: build on dev.lan, copy to the dev VM, snapshot, load, show logs.
#   scripts/loop.sh RippleHello [MAKE-TARGET]     MAKE-TARGET defaults to the lowercased name
#                                                 without "Ripple" (RippleHello -> hello)
set -eu
here=$(dirname "$0")
name=${1:?usage: loop.sh KEXT-NAME [MAKE-TARGET]}
target=${2:-$(printf '%s' "${name#Ripple}" | tr 'A-Z' 'a-z')}

start=$(date +%s)
"$here/build.sh" "$target" | grep -vE '^(clang|/home)|warning: directory not found|is deprecated' || true
"$here/deploy.sh" "$name"
echo "--- kernel log"
"$here/logs.sh" "$name" | grep kernel || true
echo "loop took $(( $(date +%s) - start )) s"
