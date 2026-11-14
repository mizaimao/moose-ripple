#!/bin/bash
# Make a ready Mac OS X 10.6.8 disk from your own install DVD image and the 10.6.8 combo update:
# installed unattended in a headless QEMU VM and never booted, so it starts at the Setup Assistant.
#
#   make-sl-disk.sh --dvd "Mac OS X Install DVD.dmg" --combo MacOSXUpdCombo10.6.8.dmg \
#                   --opencore moose-ripple-VERSION.qcow2 --osk-file FILE [--work DIR] OUT.qcow2
#
#   --dvd       a Mac OS X 10.6 install DVD as .dmg (or a raw image of the whole disc)
#   --combo     MacOSXUpdCombo10.6.8.dmg (or a raw image of it)
#   --opencore  moose-ripple's release disk; it boots the installer and is only read
#   --osk-file  a file holding the SMC key of a real Mac on one line (QEMU's isa-applesmc osk=)
#   --work      scratch folder, about 25 GB (default: sl-disk-work next to OUT); deleted at the
#               end unless KEEP_WORK=1
#
# Needs: qemu-system-x86_64 with KVM, qemu-img, OVMF, 7z (7-Zip or p7zip) and perl. No root.
#
# How it works: the DVD's own startup script, /etc/rc.install, has an unattended mode. A copy of
# the DVD gets tools/sl-disk/rc.install in its place, written over the file's bytes at the same
# length, so no HFS+ driver is needed. It partitions the empty disk, runs Apple's installer from
# an automation file, applies the combo update with the new system's own `installer`, writes the
# result to a small result disk and powers the VM off.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
die() { echo "make-sl-disk: $*" >&2; exit 1; }
say() { echo "make-sl-disk: $*" >&2; }

dvd= combo= oc= osk= work= out=
while [ $# -gt 0 ]; do
    case "$1" in
        --dvd) dvd=$2; shift 2 ;;
        --combo) combo=$2; shift 2 ;;
        --opencore) oc=$2; shift 2 ;;
        --osk-file) osk=$2; shift 2 ;;
        --work) work=$2; shift 2 ;;
        -*) die "unknown option $1" ;;
        *) out=$1; shift ;;
    esac
done
[ -n "$dvd" ] && [ -n "$combo" ] && [ -n "$oc" ] && [ -n "$osk" ] && [ -n "$out" ] ||
    die "usage: make-sl-disk.sh --dvd DVD --combo COMBO --opencore DISK --osk-file FILE OUT.qcow2"
for f in "$dvd" "$combo" "$oc" "$osk"; do [ -f "$f" ] || die "no such file: $f"; done
[ -e "$out" ] && die "$out exists"
SEVENZ=$(command -v 7z || command -v 7zz || true)
[ -n "$SEVENZ" ] || die "needs 7z (7-Zip or p7zip)"
for t in qemu-system-x86_64 qemu-img perl; do command -v $t >/dev/null || die "needs $t"; done
[ -w /dev/kvm ] || die "needs KVM: /dev/kvm isn't writable for you (add yourself to the kvm group)"
code=
for c in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd \
         /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/qemu/OVMF_CODE.fd; do
    [ -f "$c" ] && { code=$c; break; }
done
[ -n "$code" ] || die "needs OVMF (no OVMF_CODE*.fd found)"
vars=${code/CODE/VARS}
[ -f "$vars" ] || die "needs OVMF's VARS file next to $code"
key=$(head -1 "$osk" | tr -d '\r\n"')
[ ${#key} -eq 64 ] || die "the SMC key in $osk should be 64 characters"

work=${work:-$(dirname "$out")/sl-disk-work}
mkdir -p "$work"
work=$(cd "$work" && pwd)
rm -rf "$work/parts" "$work/rc"
mkdir "$work/parts" "$work/rc"

say "unpacking the DVD image"
case "$dvd" in *.dmg|*.DMG) t=-tdmg ;; *) t= ;; esac
"$SEVENZ" x $t -y -o"$work/parts" "$dvd" >/dev/null 2>&1 || die "7z could not read $dvd"
hfs=$(ls -S "$work/parts"/*.hfs "$work/parts"/*.hfsx 2>/dev/null | head -1 || true)
[ -n "$hfs" ] || die "no HFS+ partition in $dvd"
"$SEVENZ" x -y -o"$work/rc" "$hfs" '-ir!*/private/etc/rc.install' >/dev/null 2>&1 || true
orig=$(find "$work/rc" -name rc.install -type f | head -1)
[ -n "$orig" ] || die "no /etc/rc.install on $dvd: is it a Mac OS X 10.6 install DVD?"
grep -q MINSTALL_CONF "$orig" || die "this DVD's rc.install has no unattended mode (not a 10.6 DVD?)"
case "$dvd" in
    *.dmg|*.DMG) ls "$work/parts" | sort -n | while read -r p; do cat "$work/parts/$p"; done > "$work/dvd.img" ;;
    *) cp "$dvd" "$work/dvd.img" ;;
esac
rm -rf "$work/parts"

# Write our rc.install over the original's bytes: found once in the image, padded to its length.
perl -e '
    my ($img, $old, $new) = @ARGV; local $/;
    open(my $o, "<:raw", $old) or die "$old: $!\n"; my $a = <$o>;
    open(my $n, "<:raw", $new) or die "$new: $!\n"; my $b = <$n>;
    my $pad = length($a) - length($b);
    $pad >= 0 or die "our rc.install is longer than the DVD'"'"'s\n";
    $b .= $pad >= 2 ? ("#" x ($pad - 1)) . "\n" : "\n" x $pad;
    open(my $f, "+<:raw", $img) or die "$img: $!\n";
    my ($buf, $base, @hits) = ("", 0);
    while (read($f, my $blk, 64 << 20)) {
        $buf .= $blk;
        for (my $i = index($buf, $a); $i >= 0; $i = index($buf, $a, $i + 1)) { push @hits, $base + $i }
        my $drop = length($buf) - (length($a) - 1);
        if ($drop > 0) { $buf = substr($buf, $drop); $base += $drop }
    }
    @hits == 1 or die "rc.install found " . scalar(@hits) . " times in the image, expected once\n";
    seek($f, $hits[0], 0) or die; print $f $b; close($f) or die "$img: $!\n";
    printf STDERR "make-sl-disk: rc.install replaced at byte %d (%d bytes)\n", $hits[0], length($a);
' "$work/dvd.img" "$orig" "$here/rc.install"
rm -rf "$work/rc"

say "unpacking the combo update"
case "$combo" in
    *.dmg|*.DMG)
        mkdir "$work/parts"
        "$SEVENZ" x -tdmg -y -o"$work/parts" "$combo" >/dev/null 2>&1 || die "7z could not read $combo"
        ls "$work/parts" | sort -n | while read -r p; do cat "$work/parts/$p"; done > "$work/combo.img"
        rm -rf "$work/parts" ;;
    *) cp "$combo" "$work/combo.img" ;;
esac
qemu-img create -q -f qcow2 "$work/target.qcow2" 64G
rm -f "$work/result.img"; truncate -s 16M "$work/result.img"
# Pass 1 installs Mac OS X; Apple's installer then restarts the VM, which ends QEMU (-no-reboot).
# One CPU: with two, 10.6.3's installer kernel sometimes panicked at boot with
# "pmap_flush_tlbs() timeout" (a CPU not answering the TLB shootdown, as in KI-5).
# Pass 2 boots the DVD again with fresh firmware settings (so OpenCore picks the DVD, the first
# disk after its own, and never the new system) and applies the combo update.
for pass in 1 2; do
cp "$vars" "$work/vars.fd"
say "pass $pass: installing in a headless VM (pass 1 about 10-40 minutes, pass 2 about 3-15)"
limit=$([ $pass = 1 ] && echo 3600 || echo 1800)
# A screenshot every 2 minutes into the work folder (QEMU's screendump, PPM), to see a stall.
( while sleep 120; do
      [ -S "$work/monitor.sock" ] || continue
      printf 'screendump %s/pass%s-%s.ppm\n' "$work" $pass "$(date +%H%M%S)" |
          perl -MIO::Socket::UNIX -e '$s = IO::Socket::UNIX->new(Peer => $ARGV[0]) or exit;
              sleep 1; print $s <STDIN>; sleep 2' "$work/monitor.sock" 2>/dev/null
  done ) &
shots=$!
timeout $limit qemu-system-x86_64 \
    -machine pc-q35-7.2,accel=kvm -m 4096 -smp 1 \
    -cpu Penryn,kvm=on,vendor=GenuineIntel,+sse4.1,+sse4.2,+ssse3,vmware-cpuid-freq=off \
    -device isa-applesmc,osk="$key" -smbios type=2 \
    -global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off \
    -drive if=pflash,format=raw,readonly=on,file="$code" -drive if=pflash,format=raw,file="$work/vars.fd" \
    -fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=0 \
    -vga std -display none -no-reboot -nic none \
    -monitor unix:"$work/monitor.sock",server,nowait \
    -device ich9-usb-ehci1,id=ehci -device usb-kbd,bus=ehci.0 -device usb-tablet,bus=ehci.0 \
    -drive file="$oc",if=none,id=oc,snapshot=on -device ide-hd,bus=ide.0,drive=oc,bootindex=0 \
    -drive file="$work/target.qcow2",if=none,id=t,format=qcow2 -device ide-hd,bus=ide.2,drive=t \
    -drive file="$work/dvd.img",if=none,id=dvd,format=raw,snapshot=on -device ide-hd,bus=ide.1,drive=dvd \
    -drive file="$work/combo.img",if=none,id=combo,format=raw,snapshot=on -device ide-hd,bus=ide.3,drive=combo \
    -drive file="$work/result.img",if=none,id=res,format=raw -device ide-hd,bus=ide.4,drive=res \
    && rc=0 || rc=$?
kill $shots 2>/dev/null || true
head -c 16 "$work/result.img" | tr -d '\0' | grep -q SLREADY && break
# Pass 1 should end with the installer's restart (QEMU exits 0 under -no-reboot). A timeout or
# crash there leaves no system to update: stop rather than run pass 2.
[ $rc = 0 ] || die "pass $pass ended without a result (QEMU exit $rc; screenshots in $work)"
done

result=$(head -c 512 "$work/result.img" | tr -d '\0' | head -1)
perl -e 'local $/; $_ = <>; s/\0+//g; print' "$work/result.img" > "${out%.qcow2}-install.log" 2>/dev/null || true
case "$result" in
    "SLREADY OK 10K549") ;;
    "") die "no result from the VM (it didn't finish; work folder: $work)" ;;
    *) die "the install ended with: ${result#SLREADY }; installer log: ${out%.qcow2}-install.log" ;;
esac
say "compressing"
qemu-img convert -c -O qcow2 "$work/target.qcow2" "$out"
say "done: $out (Mac OS X 10.6.8 10K549, never booted)"
[ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$work"
