#!/bin/sh
# Cross-build the Windows 7 UI regression and timing probes on Linux.
# LLVM_MINGW: toolchain prefix (directory containing bin/); OUT: executable output.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
LLVM_MINGW=${LLVM_MINGW:-$root/.tools/llvm-mingw/llvm-mingw-20261104-msvcrt-ubuntu-22.04-x86_64}
OUT=${OUT:-$root/build/win7-ui-probes}
mkdir -p "$OUT"
for arch in x86_64 i686; do
    case $arch in x86_64) suffix= ;; i686) suffix=32 ;; esac
    cc=$LLVM_MINGW/bin/$arch-w64-mingw32-clang
    "$cc" -O2 -Wall -Wextra -Werror "$here/d3d9upload.c" -ld3d9 -lgdi32 -o "$OUT/d3d9upload$suffix.exe"
done
cc=$LLVM_MINGW/bin/x86_64-w64-mingw32-clang
"$cc" -O2 -Wall -Wextra -Werror "$here/shellmenu.c" -o "$OUT/shellmenu.exe"
"$cc" -O2 -Wall -Wextra -Werror "$here/dwmtiming.c" -ldwmapi -lgdi32 -o "$OUT/dwmtiming.exe"
"$cc" -O2 -Wall -Wextra -Werror "$here/timerbench.c" -o "$OUT/timerbench.exe"
