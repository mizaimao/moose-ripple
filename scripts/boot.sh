#!/bin/sh
# Start the dev VM and wait for ssh.
#   scripts/boot.sh                  OpenCore (bios: ovmf): boots by itself; flags live in NVRAM
#                                    (scripts/bootargs.sh). Chameleon (SeaBIOS): boots with -v.
#   scripts/boot.sh '-v arch=i386'   Chameleon only: flags typed at its prompt
set -eu
. "$(dirname "$0")/lib.sh"
here=$(dirname "$0")

flags=${1--v}
status=$(ssh "$RETRO" qm status "$VMID")
[ "$status" = "status: stopped" ] || die "VM $VMID is not stopped ($status); run scripts/stop.sh first"
fw=$(ssh "$RETRO" "qm config $VMID | sed -n 's/^bios: //p'")

ssh "$RETRO" qm start "$VMID"
if [ "$fw" = ovmf ]; then
    # OpenCore shows its menu for 5 s and starts Macintosh HD (decision 0007).
    [ "$flags" = -v ] || echo "note: with OpenCore, set boot flags with scripts/bootargs.sh; '$flags' ignored" >&2
else
    sleep 8                                   # Chameleon waits at its menu for a key
    ssh "$RETRO" qm sendkey "$VMID" right     # from the Chameleon CD entry to Macintosh HD
    [ -z "$flags" ] || "$here/type.sh" "$flags"
    ssh "$RETRO" qm sendkey "$VMID" ret
fi

i=0
while :; do
    sleep 5
    addr=$(guest_addr)
    if [ -n "$addr" ] && GUEST=$addr gssh true 2>/dev/null; then break; fi
    i=$((i + 1))
    [ "$i" -lt 36 ] || die "no ssh after 180 s; look with scripts/screen.sh"
done
echo "VM $VMID up at $addr: $(GUEST=$addr gssh 'sw_vers -productVersion; uname -m' | tr '\n' ' ')"
