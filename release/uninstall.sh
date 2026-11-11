#!/bin/sh
# moose-ripple uninstaller, run inside Mac OS X 10.6.8:  sudo sh uninstall.sh
# Removes what install.sh put in /System/Library/Extensions, rebuilds the kext cache and takes
# ripple_gl/ripple_qe out of the boot flags (npci=0x2000 stays: OpenCore's VM setup needs it).
# Restart afterwards.
set -eu
EXT=/System/Library/Extensions
die() { echo "uninstall.sh: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run it as root: sudo sh uninstall.sh"

for i in RippleVirtioGPU.kext RippleGLDriver.bundle RippleGA.plugin RippleQEMUTablet.kext; do
    if [ -e "$EXT/$i" ]; then rm -rf "$EXT/$i" && echo "removed $EXT/$i"; fi
done
echo "Rebuilding the kext cache"
# Run from /: started from a folder on the CD, kextcache fails ("Couldn't lock ...: Operation
# not supported by device").
cd /
kc=$(kextcache -system-mkext 2>&1) || die "kextcache -system-mkext failed: $(echo "$kc" | tail -1)"

cur=$(nvram boot-args 2>/dev/null | sed 's/^boot-args[[:space:]]*//' || true)
new=
for f in $cur; do
    case "$f" in ripple_*=*) ;; *) new="$new $f" ;; esac
done
new="${new# }"
nvram boot-args="$new"
echo "Boot flags: $new"
echo "Done. Restart to finish: sudo reboot"
