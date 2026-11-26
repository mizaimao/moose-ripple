#!/bin/sh
# Run on the VM host, as root, from a folder of its own that survives a restart of the host
# (with guestctl.pl, the CD image ref.iso, a prepared disk and its data disk next to it).
# MIT licence (LICENSE at the repository root).
# A Windows 7 or Vista guest for comparing our driver with a real graphics card, like for
# like: the same disk, CPU model, cores and memory, once with the virtio GPU (virgl) and once
# with the card passed through as Proxmox passes it to the owner's VM of that system. Nothing
# of the host or its VMs changes; each run gets its own throwaway overlay over the prepared disk.
#   sh refvm.sh new NAME          a fresh overlay NAME.qcow2 over the prepared disk
#   sh refvm.sh virgl NAME        start it with the virtio GPU (picture on NAME-vnc.sock)
#   sh refvm.sh card NAME         start it with the card (no picture: keys through QMP only)
#   sh refvm.sh kill NAME         end QEMU (only after a shutdown in the guest failed)
# REF picks the prepared disk: w7ref (Windows 7, the default) or vref (Vista), for
# REF-base.qcow2 and REF-data.raw. ISO names another CD image than ref.iso.
# DISKOPT is added to the system disk's drive options (",cache=unsafe" keeps the host from
# the disk on the guest's flushes; for throwaway overlays only).
# Both starts refuse while any other guest runs on the host (SHARED=1 skips that, for an
# untimed check on a host that is shared). Before "card": no guest on the host for several
# minutes, and the kernel log streaming to another machine.
set -eu
umask 077
cd "$(dirname "$0")"
here=$PWD
step=$1; name=$2
ref=${REF:-w7ref}
iso=${ISO:-ref.iso}
rom=/usr/share/kvm/9600gt-tpu.bin
case "$ref" in
vref)
    # Vista: the owner's VM has no hv_relaxed, and its card sits behind Proxmox's first PCIe
    # root port (pcie=1)
    cpu='Penryn,enforce,hv_reset,hv_runtime,hv_spinlocks=0x1fff,hv_time,hv_vapic,hv_vendor_id=proxmox,hv_vpindex,kvm=off,+kvm_pv_eoi,+kvm_pv_unhalt,vendor=GenuineIntel'
    card="-device ioh3420,id=ich9-pcie-port-1,multifunction=on,bus=pcie.0,addr=1c.0,port=1,chassis=1 -device vfio-pci,host=0000:01:00.0,id=hostpci0,bus=ich9-pcie-port-1,addr=0x0,x-vga=on,romfile=$rom"
    ;;
w7ref)
    cpu='Penryn,enforce,hv_relaxed,hv_reset,hv_runtime,hv_spinlocks=0x1fff,hv_time,hv_vapic,hv_vendor_id=proxmox,hv_vpindex,kvm=off,+kvm_pv_eoi,+kvm_pv_unhalt,vendor=GenuineIntel'
    card="-device vfio-pci,host=0000:01:00.0,id=hostpci0,bus=pcie.0,addr=0x10,x-vga=on,romfile=$rom"
    ;;
*) exit 2 ;;
esac
occupied() {
    if command -v qm >/dev/null 2>&1 && qm list | awk 'NR>1 && $3=="running"{n++} END{exit !n}'; then return 0; fi
    ps -eo comm= | grep -Eq '^(kvm$|qemu-system-)'
}
case "$step" in
new)
    test -s "$ref-base.qcow2" && test ! -e "$name.qcow2"
    qemu-img create -q -f qcow2 -F qcow2 -b "$here/$ref-base.qcow2" "$name.qcow2"
    ;;
virgl|card)
    if test -z "${SHARED:-}" && occupied; then echo 'the host has a guest running'; exit 3; fi
    test -s "$name.qcow2" && test -s "$iso" && test -s "$ref-data.raw"
    f=/sys/class/drm/card0/gt_min_freq_mhz
    if test -w "$f" && test "$(cat $f)" = 350; then
        echo 1200 >"$f"
        for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance >"$g"; done
    fi
    if test "$step" = virgl; then
        display="-device virtio-vga-gl,id=vgpu,xres=1280,yres=1024,edid=on -display egl-headless,rendernode=/dev/dri/renderD128 -vnc unix:$here/$name-vnc.sock"
    else
        display="$card -display none"
    fi
    : >"$name-com1.log"
    # shellcheck disable=SC2086
    qemu-system-x86_64 -name "ripple-ref-$name" -no-user-config -nodefaults \
        -machine pc-q35-2.10,accel=kvm,hpet=off,smm=off -cpu "$cpu" -rtc driftfix=slew,base=localtime \
        -global kvm-pit.lost_tick_policy=discard \
        -smp 4,sockets=1,cores=4,maxcpus=4 -m 4096 \
        -nic none -vga none $display \
        -device ich9-usb-uhci1,id=uhci -device usb-tablet,bus=uhci.0 \
        -drive "file=$here/$name.qcow2,format=qcow2,if=none,id=guestdisk${DISKOPT:-}" -device ide-hd,drive=guestdisk,bus=ide.0,bootindex=0 \
        -drive "file=$here/$iso,media=cdrom,readonly=on,if=none,id=labcd" -device ide-cd,drive=labcd,bus=ide.1,bootindex=1 \
        -drive "file=$here/$ref-data.raw,format=raw,if=none,id=datadisk" -device ide-hd,drive=datadisk,bus=ide.2 \
        -serial "file:$here/$name-com1.log" -qmp "unix:$here/$name-qmp.sock,server=on,wait=off" \
        -msg timestamp=on -pidfile "$here/$name.pid" -D "$here/$name-qemu.log" -daemonize
    echo "started $(cat "$name.pid") ($step) at $(date +%T)"
    ;;
kill)
    kill "$(cat "$name.pid")"
    ;;
*) exit 2 ;;
esac
