#!/bin/sh
# moose-ripple installer, run inside Mac OS X 10.6.8 from the unpacked package folder:
#   sudo sh install.sh [--no-tablet] [--sound] [--hwcursor] [--logs] [--target VOLUME]
#     (no options)  the drivers, the GL bundle, the GA plugin and the tablet kext
#     --no-tablet   without RippleQEMUTablet.kext
#     --sound       with RippleVirtioSound.kext, the driver for QEMU's virtio-sound-pci device
#     --hwcursor    the pointer as a hardware cursor (boot flag ripple_hwcursor=1); UTM shows the
#                   pointer only that way while Quartz Extreme is on
#     --logs        per-program logs of the GL driver in /Library/Logs/RippleGL
#     --target VOLUME
#                   install onto another 10.6.8 system, e.g. a new one that has never started,
#                   so that its first boot (the Welcome animation) already has moose-ripple.
#                   Run from a 10.6 system with that disk attached, or from the install DVD.
#                   GL, Quartz Extreme and --hwcursor are then switched on in the driver's own
#                   settings, as boot flags belong to the VM and not to the disk.
# Copies the bundles into /System/Library/Extensions (root:wheel, read-only for others), checks
# the kernel drivers, rebuilds the kext cache, turns sleep off and sets the boot flags. Restart
# afterwards. Take a snapshot of the VM first. uninstall.sh undoes the drivers and the flags.
# Written for the install DVD's shell too, which has no grep or sed: awk and perl instead.
set -eu
case "$0" in */*) cd "${0%/*}" ;; esac
PKG=$(pwd)
die() { echo "install.sh: $*" >&2; exit 1; }

ITEMS="RippleVirtioGPU.kext RippleGLDriver.bundle RippleGA.plugin"
TABLET=1 SOUND=0 HWCURSOR=0 LOGS=0 TARGET=
while [ $# -gt 0 ]; do
    case "$1" in
        --no-tablet) TABLET=0 ;;
        --sound)     SOUND=1 ;;
        --hwcursor)  HWCURSOR=1 ;;
        --logs)      LOGS=1 ;;
        --target)    [ $# -ge 2 ] || die "--target needs a volume, e.g. --target \"/Volumes/Macintosh HD\""
                     TARGET=$2; shift ;;
        *) die "unknown option $1 (options: --no-tablet --sound --hwcursor --logs --target VOLUME)" ;;
    esac
    shift
done
[ "$TABLET" = 0 ] || ITEMS="$ITEMS RippleQEMUTablet.kext"
[ "$SOUND" = 0 ] || ITEMS="$ITEMS RippleVirtioSound.kext"
# With --target, tools the running system lacks (the install DVD's shell) come from the target's.
[ -z "$TARGET" ] || PATH=$PATH:$TARGET/usr/bin:$TARGET/bin:$TARGET/usr/sbin:$TARGET/sbin

[ "$(id -u)" = 0 ] || die "run it as root: sudo sh install.sh"
if [ -n "$TARGET" ]; then
    [ -d "$TARGET/System/Library/Extensions" ] || die "$TARGET has no Mac OS X system on it"
    TARGET=$(cd "$TARGET" && pwd -P)
    [ "$TARGET" != / ] || die "--target is for another disk; for the running system leave it out"
    v=$(perl -0ne 'print $1 if m|ProductVersion</key>\s*<string>([\d.]+)|' \
        "$TARGET/System/Library/CoreServices/SystemVersion.plist" 2>/dev/null || true)
    [ "$v" = 10.6.8 ] || die "$TARGET has Mac OS X ${v:-of an unknown version}; it needs 10.6.8"
    # A disk attached to a running system may have ownership off; 10.6 needs root:wheel kexts.
    case "$(diskutil info "$TARGET" 2>/dev/null | awk '/Owners:/ {print $2}')" in
        Disabled) diskutil enableOwnership "$TARGET" >/dev/null && echo "Turned ownership on for $TARGET" ;;
    esac
else
    v=$(sw_vers -productVersion)
    [ "$v" = 10.6.8 ] || die "needs Mac OS X 10.6.8, this is $v"
    [ "$(uname -m)" = x86_64 ] || die "needs the 64-bit kernel (uname -m says $(uname -m)); boot through OpenCore"
fi
EXT=$TARGET/System/Library/Extensions
for i in $ITEMS; do [ -d "$i" ] || die "$i is missing from $PKG"; done

echo "Checking the files against SHA256SUMS"
# shasum is a perl script; the install DVD has neither it nor the perl modules it needs, so with
# --target the target's script and modules are used.
sums() {
    if [ -n "$TARGET" ] && [ -f "$TARGET/usr/bin/shasum5.10.0" ]; then
        PERL5LIB="$TARGET/System/Library/Perl/5.10.0/darwin-thread-multi-2level:$TARGET/System/Library/Perl/5.10.0" \
            perl "$TARGET/usr/bin/shasum5.10.0" "$@"
    else
        shasum "$@"
    fi
}
res=$(sums -a 256 -c SHA256SUMS 2>&1 || true)
bad=$(echo "$res" | awk '/: FAILED/')
[ -z "$bad" ] || die "checksum mismatch, download again: $bad"
case "$res" in
    *": OK"*) ;;
    *) echo "install.sh: shasum doesn't run here, the checksums are not checked: $(echo "$res" | awk 'NR == 1')" >&2 ;;
esac

for i in $ITEMS; do
    rm -rf "$EXT/$i"
    cp -R "$i" "$EXT/"
    chown -R root:wheel "$EXT/$i"
    chmod -R u=rwX,go=rX "$EXT/$i"
    [ ! -d "$EXT/$i/Contents/MacOS" ] || chmod 755 "$EXT/$i"/Contents/MacOS/*
    echo "installed $EXT/$i"
done

KEXTS=RippleVirtioGPU.kext
[ "$SOUND" = 0 ] || KEXTS="$KEXTS RippleVirtioSound.kext"
for k in $KEXTS; do
    if ! command -v kextutil >/dev/null 2>&1; then
        echo "install.sh: no kextutil here; $k is not checked" >&2
        continue
    fi
    out=$(kextutil -n -t "$EXT/$k" 2>&1 || true)
    case "$out" in
        *"appears to be loadable"*) echo "$k appears to be loadable" ;;
        *) echo "$out" >&2; die "kextutil doesn't accept $k (see above); nothing else changed, run uninstall.sh" ;;
    esac
done

if [ -n "$TARGET" ]; then
    # The driver's own settings (personality properties) instead of boot flags; boot flags still
    # override them (ripple_gl=0 switches GL off again).
    PB=/usr/libexec/PlistBuddy
    [ -x "$PB" ] || PB=$TARGET/usr/libexec/PlistBuddy
    p=$EXT/RippleVirtioGPU.kext/Contents/Info.plist
    "$PB" -c "Set :IOKitPersonalities:RippleAccelerator:RippleGLBundle true" \
          -c "Set :IOKitPersonalities:RippleAccelerator:RippleQuartzExtreme true" "$p" >/dev/null
    [ "$HWCURSOR" = 0 ] || "$PB" -c "Set :IOKitPersonalities:RippleFramebuffer:RippleHardwareCursor true" "$p" >/dev/null
    # The booter reads kext Info.plists as XML only.
    plutil -convert xml1 "$p" >/dev/null
    chown root:wheel "$p"; chmod 644 "$p"
    echo "GL and Quartz Extreme on$([ "$HWCURSOR" = 0 ] || echo ", hardware cursor on") in RippleVirtioGPU.kext's settings"
fi

echo "Rebuilding the kext cache"
# Run from /: started from a folder on the CD, kextcache fails ("Couldn't lock ...: Operation
# not supported by device").
cd /
if [ -n "$TARGET" ]; then
    # If it can't be rebuilt here, the first boot rebuilds it (that boot is slower).
    touch "$EXT"
    kextcache -u "$TARGET" >/dev/null 2>&1 || echo "install.sh: kextcache -u failed; the first boot of $TARGET rebuilds the cache" >&2
else
    kc=$(kextcache -system-mkext 2>&1) || die "kextcache -system-mkext failed: $(echo "$kc" | tail -1)"
fi

# Sleep off: 10.6.8 in QEMU panics when it goes to sleep with more than one CPU (KI-6).
if [ -n "$TARGET" ]; then
    # What pmset -a writes in a VM (AC power only): the three timers in the custom profile, and
    # the custom profile (-1) as the active one.
    pm=$TARGET/Library/Preferences/SystemConfiguration/com.apple.PowerManagement.plist
    for d in :Custom\\\ Profile :Custom\\\ Profile:AC\\\ Power :ActivePowerProfiles; do
        "$PB" -c "Add $d dict" "$pm" >/dev/null 2>&1 || true
    done
    for k in :Custom\\\ Profile:AC\\\ Power:System\\\ Sleep\\\ Timer :Custom\\\ Profile:AC\\\ Power:Display\\\ Sleep\\\ Timer \
             :Custom\\\ Profile:AC\\\ Power:Disk\\\ Sleep\\\ Timer; do
        "$PB" -c "Delete $k" "$pm" >/dev/null 2>&1 || true
        "$PB" -c "Add $k integer 0" "$pm" >/dev/null
    done
    "$PB" -c "Delete :ActivePowerProfiles:AC\ Power" "$pm" >/dev/null 2>&1 || true
    "$PB" -c "Add :ActivePowerProfiles:AC\ Power integer -1" "$pm" >/dev/null
    chown root:wheel "$pm"; chmod 644 "$pm"
    echo "Sleep turned off ($pm)"
elif pmset -a sleep 0 displaysleep 0 disksleep 0; then
    echo "Sleep turned off (pmset)"
else
    echo "install.sh: pmset failed; turn sleep off by hand: pmset -a sleep 0 displaysleep 0 disksleep 0" >&2
fi
if [ "$LOGS" = 1 ]; then
    # The GL driver writes PROGRAM-PID.log here for every program while the folder exists.
    mkdir -p "$TARGET/Library/Logs/RippleGL" && chmod 1777 "$TARGET/Library/Logs/RippleGL"
    echo "GL driver logs on: /Library/Logs/RippleGL (remove the folder to stop them)"
fi

if [ -n "$TARGET" ]; then
    echo "Done. The VM that boots $TARGET needs npci=0x2000 in its boot flags (the release disk's"
    echo "OpenCore sets it when the firmware has no boot flags yet)."
    exit 0
fi

# Boot flags: keep the current ones, drop older ripple_gl/ripple_qe/npci values and -s (left
# over from installing in single-user mode, it would make every boot single-user), then add
# npci=0x2000 (PCI memory below 4 GB, needed) and ripple_gl=1 ripple_qe=1.
cur=$(nvram boot-args 2>/dev/null | awk '{ sub(/^boot-args[ \t]*/, ""); print }' || true)
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
