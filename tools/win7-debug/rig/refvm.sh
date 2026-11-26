#!/bin/sh
# Run on the VM host, as root, from a folder of its own that survives a restart of the host
# (with guestctl.pl, the CD image ref.iso, the prepared disk w7ref-base.qcow2 and the data
# disk w7ref-data.raw next to it). MIT licence (LICENSE at the repository root).
# A Windows 7 guest for comparing our driver with a real graphics card, like for like: the same
# disk, CPU model, cores and memory, once with the virtio GPU (virgl) and once with the card
# passed through as Proxmox passes it to VM 100. Nothing of the host or its VMs changes; each
# run gets its own throwaway overlay over the prepared disk.
#   sh refvm.sh new NAME          a fresh overlay NAME.qcow2 over w7ref-base.qcow2
#   sh refvm.sh virgl NAME        start it with the virtio GPU (picture on NAME-vnc.sock)
#   sh refvm.sh card NAME         start it with the card (no picture: keys through QMP only)
#   sh refvm.sh kill NAME         end QEMU (only after a shutdown in the guest failed)
# DISKOPT adds drive options to the guest disk (DISKOPT=,cache=unsafe: the host does not wait for
# the disk on the guest's flushes; for throwaway overlays only).
# Both starts refuse while any other guest runs on the host. Before "card": no guest on the
# host for several minutes, and the kernel log streaming to another machine.
set -eu
umask 077
cd "$(dirname "$0")"
here=$PWD
step=$1; name=$2
cpu='Penryn,enforce,hv_relaxed,hv_reset,hv_runtime,hv_spinlocks=0x1fff,hv_time,hv_vapic,hv_vendor_id=proxmox,hv_vpindex,kvm=off,+kvm_pv_eoi,+kvm_pv_unhalt,vendor=GenuineIntel'
occupied() {
    qm list | awk 'NR>1 && $3=="running"{n++} END{exit !n}' || ps -eo comm= | grep -Eq '^(kvm$|qemu-system-)'
}
case "$step" in
new)
    test -s w7ref-base.qcow2 && test ! -e "$name.qcow2"
    qemu-img create -q -f qcow2 -F qcow2 -b "$here/w7ref-base.qcow2" "$name.qcow2"
    ;;
virgl|card)
    occupied && { echo 'the host has a guest running'; exit 3; }
    test -s "$name.qcow2" && test -s ref.iso && test -s w7ref-data.raw
    g=$(cat /sys/class/drm/card0/gt_min_freq_mhz)
    if test "$g" = 350; then
        echo 1200 >/sys/class/drm/card0/gt_min_freq_mhz
        for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance >"$f"; done
    fi
    if test "$step" = virgl; then
        set -- -device virtio-vga-gl,id=vgpu,xres=1280,yres=1024,edid=on \
               -display egl-headless,rendernode=/dev/dri/renderD128 -vnc "unix:$here/$name-vnc.sock"
    else
        set -- -device vfio-pci,host=0000:01:00.0,id=hostpci0,bus=pcie.0,addr=0x10,x-vga=on,romfile=/usr/share/kvm/9600gt-tpu.bin \
               -display none
    fi
    : >"$name-com1.log"
    qemu-system-x86_64 -name "ripple-ref-$name" -no-user-config -nodefaults \
        -machine pc-q35-2.10,accel=kvm,hpet=off,smm=off -cpu "$cpu" -rtc driftfix=slew,base=localtime \
        -global kvm-pit.lost_tick_policy=discard \
        -smp 4,sockets=1,cores=4,maxcpus=4 -m 4096 \
        -nic none -vga none "$@" \
        -device ich9-usb-uhci1,id=uhci -device usb-tablet,bus=uhci.0 \
        -drive "file=$here/$name.qcow2,format=qcow2,if=none,id=guestdisk${DISKOPT:-}" -device ide-hd,drive=guestdisk,bus=ide.0,bootindex=0 \
        -drive "file=$here/ref.iso,media=cdrom,readonly=on,if=none,id=labcd" -device ide-cd,drive=labcd,bus=ide.1,bootindex=1 \
        -drive "file=$here/w7ref-data.raw,format=raw,if=none,id=datadisk" -device ide-hd,drive=datadisk,bus=ide.2 \
        -serial "file:$here/$name-com1.log" -qmp "unix:$here/$name-qmp.sock,server=on,wait=off" \
        -msg timestamp=on -pidfile "$here/$name.pid" -D "$here/$name-qemu.log" -daemonize
    echo "started $(cat "$name.pid") ($step) at $(date +%T)"
    ;;
kill)
    kill "$(cat "$name.pid")"
    ;;
*) exit 2 ;;
esac
