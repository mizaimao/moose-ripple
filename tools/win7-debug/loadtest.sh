#!/bin/sh
# One load test of the kernel driver in the Windows 7 test VM, as in docs/windows/test-vms.md
# ("Load test, step by step"): roll the VM back to its snapshot, install the package the build
# host serves, reboot onto our driver.
#   PKG_URL=http://BUILD_HOST:8765/pkg tools/win7-debug/loadtest.sh N
# N names the run: the guest's COM1 goes to /tmp/rb-$VMID-com1-runN.log on the VM host.
# Run from the workstation the scripts/ helpers work from. Fixed waits and click positions for
# an 800x600 guest desktop; look at the screen (scripts/screen.sh, afterwards screen-vnc.sh)
# when a step seems to have gone wrong.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$root/scripts/lib.sh"
N=${1:?run number}
: "${PKG_URL:?PKG_URL=http://BUILD_HOST:PORT/pkg}"
SNAP=${SNAP:-pre-kmd}
FILES=${FILES:-viogpu3d.inf viogpu3d.sys viogpu_d3d10.dll devcon.exe setres.ps1}
HWID='PCI\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01'
log=/tmp/rb-$VMID-com1-run$N.log

wait_s() { n=0; until [ "$n" -ge "$1" ]; do n=$((n + 1)); sleep 1; done; }
key() { "$root/scripts/qmp-input.sh" "$@" >/dev/null 2>&1; }
typ() { "$root/scripts/type.sh" -n "$1" >/dev/null 2>&1; }

ssh "$RETRO" "pkill -x socat; qm stop $VMID >/dev/null 2>&1; qm rollback $VMID $SNAP >/dev/null && qm start $VMID >/dev/null && sleep 2 && \
    (nohup socat -u UNIX-CONNECT:/var/run/qemu-server/$VMID.serial0 OPEN:$log,creat,append >/dev/null 2>&1 &)"
echo "run $N: booting"
wait_s 50
key "key shift" "sleep 2" "key meta_l" "sleep 3" "key esc" "sleep 1" "key meta_l" "sleep 3"
typ 'powershell -command "Start-Process cmd -Verb RunAs"'
wait_s 8
key "click 20012 16465" "sleep 3"                       # UAC: Yes
typ "cd /d C:\\rb\\pkg & for %f in ($FILES) do @bitsadmin /transfer r$N%f /download /priority foreground $PKG_URL/%f C:\\rb\\pkg\\%f | findstr /c:\"Transfer complete\""
wait_s 60
# Windows' own display error log, kept in memory for wddump.py
typ 'cls & for %n in (1 2 3 4 5 6 7) do @reg add "HKLM\SYSTEM\CurrentControlSet\Control\Watchdog\Logging" /v %n-LevelSizeLg2 /t REG_DWORD /d 10 /f'
wait_s 4
typ "devcon update viogpu3d.inf \"$HWID\""
wait_s 10
key "click 13943 15700"                                 # "Install this driver software anyway"
wait_s 30
"$root/scripts/screen.sh" "/tmp/loadtest-$N-installed.png" >/dev/null 2>&1 || true
typ 'shutdown /r /t 0'
echo "run $N: installed (see /tmp/loadtest-$N-installed.png), rebooting"
n=0
until ssh "$RETRO" "[ \$(wc -c < $log) -gt 300 ]" 2>/dev/null || [ "$n" -ge 40 ]; do n=$((n + 1)); sleep 5; done
wait_s 45
echo "run $N: driver log $log on the VM host: $(ssh "$RETRO" "wc -c < $log") bytes"
