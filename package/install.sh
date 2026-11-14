#!/bin/sh
# moose-ripple installer, run inside Mac OS X 10.6.8 from the unpacked package folder:
#   sudo sh install.sh [--no-tablet] [--hwcursor] [--logs]
#     (no options)  the drivers, the GL bundle, the GA plugin and the tablet kext
#     --no-tablet   without RippleQEMUTablet.kext
#     --hwcursor    the pointer as a hardware cursor (boot flag ripple_hwcursor=1); UTM shows the
#                   pointer only that way while Quartz Extreme is on
#     --logs        per-program logs of the GL driver in /Library/Logs/RippleGL
# Copies the bundles into /System/Library/Extensions (root:wheel, read-only for others), checks
# the kernel driver, rebuilds the kext cache, turns sleep off and sets the boot flags. Restart
# afterwards. Take a snapshot of the VM first. uninstall.sh undoes the drivers and the flags.
set -eu
cd "$(dirname "$0")"
EXT=/System/Library/Extensions
die() { echo "install.sh: $*" >&2; exit 1; }

ITEMS="RippleVirtioGPU.kext RippleGLDriver.bundle RippleGA.plugin"
TABLET=1 HWCURSOR=0 LOGS=0
for a in "$@"; do
    case "$a" in
        --no-tablet) TABLET=0 ;;
        --hwcursor)  HWCURSOR=1 ;;
        --logs)      LOGS=1 ;;
        *) die "unknown option $a (options: --no-tablet --hwcursor --logs)" ;;
    esac
done
[ "$TABLET" = 0 ] || ITEMS="$ITEMS RippleQEMUTablet.kext"

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

# Sleep off: 10.6.8 in QEMU panics when it goes to sleep with more than one CPU (KI-6).
if pmset -a sleep 0 displaysleep 0 disksleep 0; then
    echo "Sleep turned off (pmset)"
else
    echo "install.sh: pmset failed; turn sleep off by hand: pmset -a sleep 0 displaysleep 0 disksleep 0" >&2
fi
if [ "$LOGS" = 1 ]; then
    # The GL driver writes PROGRAM-PID.log here for every program while the folder exists.
    mkdir -p /Library/Logs/RippleGL && chmod 1777 /Library/Logs/RippleGL
    echo "GL driver logs on: /Library/Logs/RippleGL (remove the folder to stop them)"
fi

# Boot flags: keep the current ones, drop older ripple_gl/ripple_qe/npci values and -s (left
# over from installing in single-user mode, it would make every boot single-user), then add
# npci=0x2000 (PCI memory below 4 GB, needed) and ripple_gl=1 ripple_qe=1.
cur=$(nvram boot-args 2>/dev/null | sed 's/^boot-args[[:space:]]*//' || true)
new=
for f in $cur; do
    case "$f" in
        ripple_gl=*|ripple_qe=*|npci=*|-s) ;;
        ripple_hwcursor=*) [ "$HWCURSOR" = 1 ] || new="$new $f" ;;
        *) new="$new $f" ;;
    esac
done
new="${new# } npci=0x2000 ripple_gl=1 ripple_qe=1"
[ "$HWCURSOR" = 0 ] || new="$new ripple_hwcursor=1"
new="${new# }"
nvram boot-args="$new"
echo "Boot flags: $new"
echo "Done. Restart to switch moose-ripple on: sudo reboot"
