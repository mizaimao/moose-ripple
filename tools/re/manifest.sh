#!/bin/sh
# Print a Markdown table of the Mach-O files under a directory of copied Apple binaries:
# path, bundle version (from the nearest Info.plist), architectures, SHA-256.
# Runs on dev.lan:  sh tools/re/manifest.sh .tools/re/10K549
set -eu
root=${1:?usage: manifest.sh DIR}
CCT=${CCT:-.tools/cctools/bin/x86_64-apple-darwin10-}
echo "| File | Version | Archs | SHA-256 |"
echo "|---|---|---|---|"
find "$root" -type f | sort | while read -r f; do
    file -b "$f" | grep -q Mach-O || continue
    rel=${f#"$root"/}
    # Nearest enclosing Info.plist: Contents/Info.plist of a bundle, or Resources/Info.plist of a framework.
    d=$(dirname "$f"); ver=""
    while [ "$d" != "$root" ] && [ "$d" != "/" ]; do
        for p in "$d/Contents/Info.plist" "$d/Resources/Info.plist" "$d/Info.plist"; do
            if [ -f "$p" ]; then
                ver=$(grep -A1 '<key>CFBundleVersion</key>' "$p" | sed -n 's/.*<string>\(.*\)<\/string>.*/\1/p' | head -1)
                break 2
            fi
        done
        d=$(dirname "$d")
    done
    archs=$(${CCT}lipo -archs "$f" 2>/dev/null || echo "?")
    sum=$(sha256sum "$f" | cut -c1-64)
    echo "| \`$rel\` | ${ver:--} | $archs | \`$sum\` |"
done
