#!/bin/sh
# Start the dev VM, boot 10.6 through Chameleon with the given flags, wait for ssh.
#   scripts/boot.sh                  boots with -v
#   scripts/boot.sh '-v arch=i386'   boots the 32-bit kernel
#   scripts/boot.sh ''               no flags
set -eu
. "$(dirname "$0")/lib.sh"
here=$(dirname "$0")

flags=${1--v}
status=$(ssh "$RETRO" qm status "$VMID")
[ "$status" = "status: stopped" ] || die "VM $VMID is not stopped ($status); run scripts/stop.sh first"

ssh "$RETRO" qm start "$VMID"
sleep 8                                       # Chameleon waits at its menu for a key
ssh "$RETRO" qm sendkey "$VMID" right         # from the Chameleon CD entry to Macintosh HD
[ -z "$flags" ] || "$here/type.sh" "$flags"
ssh "$RETRO" qm sendkey "$VMID" ret

i=0
while :; do
    sleep 5
    addr=$(guest_addr)
    if [ -n "$addr" ] && GUEST=$addr gssh true 2>/dev/null; then break; fi
    i=$((i + 1))
    [ "$i" -lt 30 ] || die "no ssh after 150 s; look with scripts/screen.sh"
done
echo "VM $VMID up at $addr: $(GUEST=$addr gssh 'sw_vers -productVersion; uname -m' | tr '\n' ' ')"
