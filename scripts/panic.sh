#!/bin/sh
# Print the guest kernel's panic message, if it has one, from guest memory (lldb through
# QEMU's gdbstub; scripts/debug.sh). Our framebuffer can't show a panic on screen once the
# kernel stops, so this is how to read it. Halts the VM briefly and always detaches.
#   scripts/panic.sh
set -eu
here=$(dirname "$0")
"$here/debug.sh" start >/dev/null
"$here/debug.sh" lldb "settings set target.max-string-summary-length 6000" \
    "p (char *)panicstr" "p (char *)&debug_buf" 2>&1 |
    sed -n '/p (char \*)panicstr/,$p' | sed 's/\\n/\n/g; s/\\t/  /g' | grep -v "^(lldb) process detach"
"$here/debug.sh" stop >/dev/null
