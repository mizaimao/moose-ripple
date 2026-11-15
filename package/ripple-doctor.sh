#!/bin/sh
# moose-ripple check, run inside Mac OS X 10.6.8 from the unpacked package folder:
#   sh ripple-doctor.sh          (with sudo it also reads the other accounts' crash reports)
# Looks for the usual causes of trouble in one go and changes nothing. One line per check:
#   ok    as it should be
#   --    for information
#   WARN  works, but something is off; the next line says what to do
#   FAIL  moose-ripple can't work like this; the next line says what to do
# Exit status: 0 without a FAIL, 1 with one. Paste the whole output into an issue when asking
# for help; the only personal data in it are account names in crash report paths.
set -u
case "$0" in */*) cd "${0%/*}" ;; esac
PKG=$(pwd)
EXT=/System/Library/Extensions
FAILS=0

ok()   { echo "  ok    $*"; }
info() { echo "  --    $*"; }
warn() { echo "  WARN  $1"; [ -z "${2:-}" ] || echo "        $2"; }
fail() { echo "  FAIL  $1"; [ -z "${2:-}" ] || echo "        $2"; FAILS=$((FAILS + 1)); }
section() { echo; echo "$*"; }

# ---- system -------------------------------------------------------------------------------
echo "moose-ripple doctor ($(date '+%Y-%m-%d %H:%M'))"
section "System"
v=$(sw_vers -productVersion 2>/dev/null)
if [ "$v" = 10.6.8 ]; then ok "Mac OS X $v"
else fail "Mac OS X ${v:-of an unknown version}" "moose-ripple needs 10.6.8 (the 10.6.8 combo update)"; fi
if [ "$(uname -m)" = x86_64 ]; then ok "64-bit kernel"
else fail "32-bit kernel ($(uname -m))" "boot through OpenCore, which starts the 64-bit kernel"; fi
[ "$(id -u)" = 0 ] || info "not run as root: other accounts' crash reports are skipped (sudo sh ripple-doctor.sh)"

# ---- boot flags ---------------------------------------------------------------------------
section "Boot flags"
args=$(sysctl -n kern.bootargs 2>/dev/null | awk '{ $1 = $1; print }')
from="this boot"
if [ -z "$args" ]; then
    args=$(nvram boot-args 2>/dev/null | awk '{ sub(/^boot-args[ \t]*/, ""); $1 = $1; print }')
    from="NVRAM"
fi
info "${args:-(none)} ($from)"
has() { case " $args " in *" $1 "*) return 0 ;; esac; return 1; }
flag() { echo " $args " | awk -v k="$1" '{ for (i = 1; i <= NF; i++) if (index($i, k "=") == 1) { print substr($i, length(k) + 2); exit } }'; }
if [ "$(flag npci)" = 0x2000 ]; then ok "npci=0x2000"
else warn "npci=0x2000 is missing" "add it (sudo sh install.sh does): without it 10.6.8 can't use PCI memory above 4 GB under OVMF"; fi
if has -s; then warn "-s: every boot is single-user" "remove it: sudo sh install.sh drops it, or set boot-args without it in nvram or OpenCore"; fi
case "$(flag ripple_gl)" in 0) warn "ripple_gl=0 switches OpenGL off" "use ripple_gl=1 (or run install.sh again)" ;; esac
case "$(flag ripple_qe)" in 0) warn "ripple_qe=0 switches Quartz Extreme off" "use ripple_qe=1 (or run install.sh again)" ;; esac
if has serial=3; then warn "serial=3: under UTM 10.6.8 hangs early in the boot with it" "remove it unless this VM is known to boot with it"; fi

# ---- the VM's devices ---------------------------------------------------------------------
section "Devices"
pci=$(ioreg -c IOPCIDevice -r -d 1 2>/dev/null)
gpu=0 sound=0
case "$pci" in *pci1af4,1050*) gpu=1 ;; esac
case "$pci" in *pci1af4,1059*) sound=1 ;; esac
if [ "$gpu" = 1 ]; then ok "virtio-gpu display card (1af4:1050)"
else fail "no virtio-gpu display card" "give the VM virtio-vga-gl (Proxmox: vga: virtio-gl; UTM: virtio-vga-gl, GPU Supported)"; fi
if [ "$sound" = 1 ]; then info "virtio-sound device (1af4:1059)"; fi

# ---- installed files and loaded drivers ---------------------------------------------------
section "Drivers"
loaded() { kextstat -l -b "$1" 2>/dev/null | awk -v b="$1" '$6 == b { f = 1 } END { exit !f }'; }
same() {   # the installed binary is this package's (when run from the package folder)
    [ -f "$PKG/$1/Contents/MacOS/$2" ] || return 0
    cmp -s "$PKG/$1/Contents/MacOS/$2" "$EXT/$1/Contents/MacOS/$2"
}
k=RippleVirtioGPU.kext
if [ ! -d "$EXT/$k" ]; then
    fail "$k is not installed" "install moose-ripple: sudo sh install.sh, then restart"
elif ! loaded com.mooseman.ripple.VirtioGPU; then
    if [ "$gpu" = 1 ]; then
        fail "$k is installed but not loaded" "see why: sudo kextutil -t $EXT/$k; then restart"
    else
        fail "$k is installed but not loaded (no virtio-gpu card, see above)"
    fi
else
    ok "$k loaded"
fi
for b in RippleGLDriver.bundle:RippleGLDriver RippleGA.plugin:RippleGA; do
    d=${b%%:*}
    if [ -d "$EXT/$d" ]; then ok "$d installed"
    else fail "$d is not installed" "Quartz Extreme needs it: sudo sh install.sh, then restart"; fi
done
for b in RippleVirtioGPU.kext:RippleVirtioGPU RippleGLDriver.bundle:RippleGLDriver RippleGA.plugin:RippleGA \
         RippleQEMUTablet.kext:RippleQEMUTablet RippleVirtioSound.kext:RippleVirtioSound; do
    d=${b%%:*} bin=${b#*:}
    [ -d "$EXT/$d" ] || continue
    same "$d" "$bin" || warn "$d differs from this package's" "install this package: sudo sh install.sh, then restart"
done
# The tablet kext has no code: it shows as the publisher of the HID driver's settings.
case "$(ioreg -c IOHIDEventDriver -r -d 1 -l 2>/dev/null)" in
    *'"IOPersonalityPublisher" = "com.mooseman.ripple.QEMUTablet"'*)
        ok "RippleQEMUTablet.kext drives QEMU's USB tablet (the pointer follows it exactly)" ;;
    *)  if [ -d "$EXT/RippleQEMUTablet.kext" ]; then info "RippleQEMUTablet.kext installed, but no QEMU USB tablet uses it"
        else info "RippleQEMUTablet.kext not installed (installed with --no-tablet?)"; fi ;;
esac
if [ "$sound" = 1 ]; then
    if loaded com.mooseman.ripple.VirtioSound; then ok "RippleVirtioSound.kext loaded"
    elif [ -d "$EXT/RippleVirtioSound.kext" ]; then warn "RippleVirtioSound.kext is installed but not loaded" "see why: sudo kextutil -t $EXT/RippleVirtioSound.kext"
    else info "no sound driver for the virtio-sound device: sudo sh install.sh --sound"; fi
elif [ -d "$EXT/RippleVirtioSound.kext" ]; then
    info "RippleVirtioSound.kext installed, but the VM has no virtio-sound device"
fi

# ---- OpenGL and Quartz Extreme ------------------------------------------------------------
section "OpenGL and Quartz Extreme"
prop() { echo "$1" | awk -v k="\"$2\"" '$1 == k { gsub(/"/, "", $3); print $3; exit }'; }
fb=$(ioreg -c RippleFramebuffer -r -d 1 -l 2>/dev/null)
acc=$(ioreg -c RippleAccelerator -r -d 1 -l 2>/dev/null)
if [ -z "$fb" ]; then
    info "no RippleFramebuffer: the driver doesn't run (see Drivers)"
elif [ -z "$acc" ]; then
    fail "the display card has no 3D (no virgl): only 2D works" \
         "use the 3D variant: Proxmox vga: virtio-gl, QEMU -device virtio-vga-gl, UTM GPU Supported"
else
    gl=$(prop "$acc" RippleGL) qe=$(prop "$acc" RippleQE)
    if [ "$gl" = on ]; then ok "OpenGL on (GPU through virgl)"
    else warn "OpenGL off" "boot with ripple_gl=1 ripple_qe=1 (sudo sh install.sh sets them)"; fi
    if [ "$qe" = on ]; then ok "Quartz Extreme on in the driver"
    else warn "Quartz Extreme off in the driver" "boot with ripple_qe=1 (sudo sh install.sh sets it)"; fi
    w=$(prop "$fb" RippleScreenWidth) h=$(prop "$fb" RippleScreenHeight) c=$(prop "$fb" RippleCursor)
    [ -z "$w" ] || info "screen ${w}x${h}"
    [ -z "$c" ] || info "pointer: $c (in UTM use the hardware one: install.sh --hwcursor)"
fi
if [ -x "$PKG/qe-check" ]; then
    out=$("$PKG/qe-check" 2>&1); st=$?
    first=$(echo "$out" | awk 'NF { print; exit }')
    case "$out" in
        *"QE on"*)  [ "$st" = 0 ] && ok "WindowServer: Quartz Extreme on (qe-check)" ||
                        warn "WindowServer: Quartz Extreme on for some displays only" "$first" ;;
        *"QE off"*) warn "WindowServer: Quartz Extreme off (qe-check)" "$first" ;;
        *) info "qe-check couldn't ask WindowServer from here (run it in the guest's Terminal): $first" ;;
    esac
fi
if [ -d /Library/Logs/RippleGL ]; then
    info "GL driver logs on: $(ls /Library/Logs/RippleGL | awk 'END { print NR }') files in /Library/Logs/RippleGL"
fi

# ---- crashes ------------------------------------------------------------------------------
section "Crash reports of the last 24 hours"
dirs=/Library/Logs/DiagnosticReports
for u in /Users/*/Library/Logs/DiagnosticReports; do [ -r "$u" ] && dirs="$dirs $u"; done
recent() { find $dirs -maxdepth 1 -name "$1" -mtime -1 2>/dev/null; }
count() { recent "$1" | awk 'END { print NR }'; }
newest() { ls -t $(recent "$1") 2>/dev/null | awk 'NR == 1'; }
nl=$(count 'loginwindow_*') nf=$(count 'fontd_*') nm=$(count 'mds_*')
nw=$(count 'WindowServer_*') nc=$(count 'configd_*') np=$(count 'Kernel_*.panic')
if [ "$nf" -gt 0 ] || [ "$nm" -gt 0 ] || [ "$nl" -gt 2 ]; then
    fail "loginwindow $nl, fontd $nf, mds $nm crashes: QEMU probably has no valid SMC key" \
         "check the key in -device isa-applesmc,osk=...; in UTM write it without quotes (known issue KI-10)"
elif [ "$nl" -gt 0 ]; then
    info "loginwindow crashed $nl time(s); once at boot comes with the known configd crash (KI-7)"
fi
if [ "$nw" -gt 0 ]; then
    warn "WindowServer crashed $nw time(s), newest: $(newest 'WindowServer_*')" \
         "to see whether Quartz Extreme causes it, boot once with ripple_qe=0; please report it with that file"
fi
[ "$nc" = 0 ] || info "configd crashed $nc time(s): known, harmless at boot (KI-3, KI-7)"
if [ "$np" -gt 0 ]; then
    warn "$np kernel panic(s), newest: $(newest 'Kernel_*.panic')" \
         "please report it with that file (a panic when the VM sleeps is known: see Power below)"
fi
[ $((nl + nf + nm + nw + np)) -gt 0 ] || ok "no crashes of loginwindow, fontd, mds or WindowServer, no panics"

# ---- power and Spotlight ------------------------------------------------------------------
section "Power and Spotlight"
pm=$(pmset -g 2>/dev/null)
s=$(echo "$pm" | awk '$1 == "sleep" { print $2 }')
ds=$(echo "$pm" | awk '$1 == "displaysleep" { print $2 }')
dk=$(echo "$pm" | awk '$1 == "disksleep" { print $2 }')
if [ "$s" = 0 ] && [ "$ds" = 0 ] && [ "$dk" = 0 ]; then ok "sleep off"
else warn "sleep on (sleep ${s:-?}, displaysleep ${ds:-?}, disksleep ${dk:-?} minutes): 10.6.8 in QEMU panics when it sleeps" \
          "sudo pmset -a sleep 0 displaysleep 0 disksleep 0 (install.sh does this; known issue KI-6)"; fi
case "$(mdutil -s / 2>/dev/null)" in
    *enabled*) info "Spotlight indexing on: first boots are slow under emulation (UTM); in a test VM: sudo mdutil -a -i off" ;;
    *disabled*) info "Spotlight indexing off" ;;
esac
md=$(ps -axo pcpu=,comm= 2>/dev/null | awk '$2 ~ /(^|\/)(mds|mdworker)$/ { s += $1 } END { printf "%d", s }')
[ "${md:-0}" -lt 50 ] || info "Spotlight is indexing now (${md}% CPU)"

echo
if [ "$FAILS" = 0 ]; then echo "No problems that stop moose-ripple."; exit 0; fi
echo "$FAILS problem(s) that stop moose-ripple: see FAIL above."
exit 1
