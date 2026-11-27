#!/bin/sh
# framecheck.sh DIR [SECONDS-BETWEEN-CAPTURES]
# Checks a sequence of screen captures of a guest (PNG files in DIR, in name order, taken at a
# steady interval through a benchmark run) for pictures that stand still, repeat or loop.
#
# A scene that is drawn and shown as it should gives a different picture at every capture. So:
#   new     a picture not seen before in the run
#   still   the same picture as the capture before (a loading screen, or frames that stopped)
#   back    a picture seen earlier with other pictures in between: the display went back to an old
#           frame (pages of a flip chain shown again without being drawn, a loop)
#   black   a picture of one colour (PNG smaller than 4 KB at these sizes)
# It prints the counts, the longest still run, every run of back or black captures with its place
# in the sequence, and the pictures shown most often. Exit status 1 when any capture went back.
set -eu
dir=${1:?directory of captures}
step=${2:-1}
cd "$dir"
ls | grep -i '\.png$' | sort > /tmp/framecheck.$$.names
[ -s /tmp/framecheck.$$.names ] || { echo "framecheck: no PNG files in $dir"; rm -f /tmp/framecheck.$$.names; exit 2; }
while read -r f; do
    printf '%s %s %s\n' "$(md5sum < "$f" | cut -c1-32)" "$(wc -c < "$f" | tr -d ' ')" "$f"
done < /tmp/framecheck.$$.names > /tmp/framecheck.$$.sums
awk -v step="$step" '
{
    n++; hash[n] = $1; size[n] = $2; name[n] = $3
    if (size[n] < 4096) kind[n] = "black"
    else if (n > 1 && hash[n] == hash[n - 1]) kind[n] = "still"
    else if ($1 in seen) kind[n] = "back"
    else kind[n] = "new"
    seen[$1]++
    count[kind[n]]++
}
END {
    printf "FRAMECHECK captures=%d new=%d still=%d back=%d black=%d distinct=%d\n", n, count["new"], count["still"],
           count["back"], count["black"], length(seen)
    # runs of one kind
    longest = 0
    for (i = 1; i <= n; i = j) {
        for (j = i; j <= n && kind[j] == kind[i]; ++j) {}
        len = j - i
        if (kind[i] == "still" && len > longest) { longest = len; at = i }
        if (kind[i] == "back" || kind[i] == "black" || (kind[i] == "still" && len * step >= 5))
            printf "FRAMECHECK %-5s %4d captures (%.0f s) from %s (%.0f s into the run)\n", kind[i], len, len * step,
                   name[i], (i - 1) * step
    }
    printf "FRAMECHECK longest_still=%d captures (%.0f s)\n", longest, longest * step
    # the pictures shown most often
    for (h in seen) if (seen[h] > 2) {
        first = ""; last = ""
        for (i = 1; i <= n; ++i) if (hash[i] == h) { if (first == "") first = name[i]; last = name[i] }
        printf "FRAMECHECK repeated %4d times: %s to %s\n", seen[h], first, last
    }
    exit count["back"] ? 1 : 0
}' /tmp/framecheck.$$.sums && status=0 || status=$?
rm -f /tmp/framecheck.$$.names /tmp/framecheck.$$.sums
exit $status
