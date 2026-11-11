#!/bin/sh
# moose-ripple installer, run inside Mac OS X 10.6.8 from the unpacked release folder:
#   sudo sh install.sh               the drivers, the GL bundle, the GA plugin and the tablet kext
#   sudo sh install.sh --no-tablet   the same without RippleQEMUTablet.kext
# Copies the bundles into /System/Library/Extensions (root:wheel, read-only for others), checks
# the kernel driver, rebuilds the kext cache and adds the boot flags. Restart afterwards.
# Take a snapshot of the VM first. uninstall.sh undoes all of it.
set -eu
cd "$(dirname "$0")"
EXT=/System/Library/Extensions
ITEMS="RippleVirtioGPU.kext RippleGLDriver.bundle RippleGA.plugin"
[ "${1:-}" = --no-tablet ] || ITEMS="$ITEMS RippleQEMUTablet.kext"

die() { echo "install.sh: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run it as root: sudo sh install.sh"
v=$(sw_vers -productVersion)
[ "$v" = 10.6.8 ] || die "needs Mac OS X 10.6.8, this is $v"
[ "$(uname -m)" = x86_64 ] || die "needs the 64-bit kernel (uname -m says $(uname -m)); boot through OpenCore"
for i in $ITEMS; do [ -d "$i" ] || die "$i is missing from $(pwd)"; done

echo "Checking the files against SHA256SUMS"
bad=$(shasum -a 256 -c SHA256SUMS 2>&1 | grep -v ': OK$' || true)
[ -z "$bad" ] || die "checksum mismatch, download again: $bad"

for i in $ITEMS; do
    rm -rf "$EXT/$i"
    cp -R "$i" "$EXT/"
    chown -R root:wheel "$EXT/$i"
    chmod -R u=rwX,go=rX "$EXT/$i"
    [ ! -d "$EXT/$i/Contents/MacOS" ] || chmod 755 "$EXT/$i"/Contents/MacOS/*
    echo "installed $EXT/$i"
done

out=$(kextutil -n -t "$EXT/RippleVirtioGPU.kext" 2>&1 || true)
case "$out" in
    *"appears to be loadable"*) echo "RippleVirtioGPU.kext appears to be loadable" ;;
    *) echo "$out" >&2; die "kextutil doesn't accept RippleVirtioGPU.kext (see above); nothing else changed, run uninstall.sh" ;;
esac
echo "Rebuilding the kext cache"
# Run from /: started from a folder on the CD, kextcache fails ("Couldn't lock ...: Operation
# not supported by device").
cd /
kc=$(kextcache -system-mkext 2>&1) || die "kextcache -system-mkext failed: $(echo "$kc" | tail -1)"

# Boot flags: keep the current ones, drop older ripple_gl/ripple_qe values, then add
# npci=0x2000 (PCI memory below 4 GB, needed) and ripple_gl=1 ripple_qe=1.
cur=$(nvram boot-args 2>/dev/null | sed 's/^boot-args[[:space:]]*//' || true)
new=
for f in $cur; do
    case "$f" in ripple_gl=*|ripple_qe=*|npci=*) ;; *) new="$new $f" ;; esac
done
new="${new# } npci=0x2000 ripple_gl=1 ripple_qe=1"
new="${new# }"
nvram boot-args="$new"
echo "Boot flags: $new"
echo "Done. Restart to switch moose-ripple on: sudo reboot"
