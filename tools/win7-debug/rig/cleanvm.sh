#!/bin/sh
# Run on the VM host, as root, from a folder of its own (with guestctl.pl and the CD image
# rel.iso next to it). MIT licence (LICENSE at the repository root).
# Clean-install check of a Vista or Windows 7 driver package (docs/windows/test-vms.md): a
# throwaway guest with the clock and CPU options Proxmox gives its Windows VMs, on a disk that
# never had the driver, a test certificate or test mode. Nothing of the host or its VMs changes:
# Vista runs in an overlay over VM 101's disk (read only), Windows 7 in an overlay over a copy
# of VM 220's snapshot clone-fresh (the disk as cloned, before its first boot).
#   sh cleanvm.sh vista|win7 prep    the disk          sh cleanvm.sh OS reset   a fresh overlay
#   sh cleanvm.sh vista|win7 boot    start the guest (refuses while any other guest runs)
#   sh cleanvm.sh vista|win7 kill    end QEMU (only after a shutdown in the guest failed)
# VISTA_DISK, WIN7_DISK and WIN7_SNAPSHOT name other sources.
set -eu
umask 077
cd "$(dirname "$0")"
here=$PWD
os=$1; step=$2
case "$os" in
    vista) cpu='Penryn,enforce,hv_reset,hv_runtime,hv_spinlocks=0x1fff,hv_time,hv_vapic,hv_vendor_id=proxmox,hv_vpindex,kvm=off,+kvm_pv_eoi,+kvm_pv_unhalt,vendor=GenuineIntel'
           base=${VISTA_DISK:-/var/lib/vz/images/101/vm-101-disk-0.qcow2} ;;
    win7)  cpu='Penryn,enforce,hv_relaxed,hv_reset,hv_runtime,hv_spinlocks=0x1fff,hv_time,hv_vapic,hv_vpindex,+kvm_pv_eoi,+kvm_pv_unhalt,vendor=GenuineIntel'
           base=$here/win7-base.qcow2 ;;
    *) exit 2 ;;
esac
occupied() {
    qm list | awk 'NR>1 && $3=="running"{n++} END{exit !n}' || ps -eo comm= | grep -Eq '^(kvm$|qemu-system-)'
}
case "$step" in
prep)
    if test "$os" = win7 && test ! -s "$base"; then
        qemu-img convert -l "snapshot.name=${WIN7_SNAPSHOT:-clone-fresh}" -O qcow2 \
            "${WIN7_DISK:-/var/lib/vz/images/220/vm-220-disk-0.qcow2}" "$base.part"
        mv "$base.part" "$base"
    fi
    test ! -e "$os.qcow2"
    qemu-img create -q -f qcow2 -F qcow2 -b "$base" "$os.qcow2"
    qemu-img info "$os.qcow2" | grep -E "virtual size|backing file:"
    ;;
reset)
    test ! -e "$os.pid" || ! kill -0 "$(cat "$os.pid")" 2>/dev/null
    rm -f "$os.qcow2"
    qemu-img create -q -f qcow2 -F qcow2 -b "$base" "$os.qcow2"
    ;;
boot)
    occupied && { echo 'the host has a guest running'; exit 3; }
    test -s "$os.qcow2" && test -s rel.iso
    g=$(cat /sys/class/drm/card0/gt_min_freq_mhz)
    if test "$g" = 350; then
        echo 1200 >/sys/class/drm/card0/gt_min_freq_mhz
        for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance >"$f"; done
    fi
    qemu-system-x86_64 -name "ripple-rel-$os" -no-user-config -nodefaults \
        -machine pc-q35-2.10,accel=kvm,hpet=off -cpu "$cpu" -rtc driftfix=slew,base=localtime \
        -global kvm-pit.lost_tick_policy=discard \
        -smp 4,sockets=1,cores=4,maxcpus=4 -m 4096 \
        -nic none -vga none -device virtio-vga-gl,id=vgpu,xres=1280,yres=1024,edid=on \
        -display egl-headless,rendernode=/dev/dri/renderD128 -vnc "unix:$PWD/$os-vnc.sock" \
        -device ich9-usb-uhci1,id=uhci -device usb-tablet,bus=uhci.0 \
        -drive "file=$PWD/$os.qcow2,format=qcow2,if=none,id=guestdisk" -device ide-hd,drive=guestdisk,bus=ide.0,bootindex=0 \
        -drive "file=$PWD/rel.iso,media=cdrom,readonly=on,if=none,id=labcd" -device ide-cd,drive=labcd,bus=ide.1,bootindex=1 \
        -serial "file:$PWD/$os-com1.log" -qmp "unix:$PWD/$os-qmp.sock,server=on,wait=off" \
        -msg timestamp=on -pidfile "$PWD/$os.pid" -D "$PWD/$os-qemu.log" -daemonize
    echo "started $(cat "$os.pid") at $(date -u +%T)"
    ;;
kill)
    kill "$(cat "$os.pid")"
    ;;
*) exit 2 ;;
esac
