#!/bin/sh
# Type text on the dev VM's console through QEMU key presses (US layout), headless.
#   scripts/type.sh 'some text'           types the text
#   scripts/type.sh -n 'some text'        types the text, then Return
#   scripts/type.sh -k ret -k meta_l-q    sends named QEMU keys (qcodes)
# Never pass passwords or keys through this: the text shows up in process lists and logs.
set -eu
. "$(dirname "$0")/lib.sh"

qcode() {
    case "$1" in
        # Spelled out: in some locales [a-z] also matches capitals.
        [ABCDEFGHIJKLMNOPQRSTUVWXYZ]) echo "shift-$(printf '%s' "$1" | tr 'A-Z' 'a-z')" ;;
        [abcdefghijklmnopqrstuvwxyz0123456789]) echo "$1" ;;
        ' ') echo spc ;;
        '-') echo minus ;;        '_') echo shift-minus ;;
        '=') echo equal ;;        '+') echo shift-equal ;;
        '/') echo slash ;;        '?') echo shift-slash ;;
        '.') echo dot ;;          '>') echo shift-dot ;;
        ',') echo comma ;;        '<') echo shift-comma ;;
        ';') echo semicolon ;;    ':') echo shift-semicolon ;;
        "'") echo apostrophe ;;   '"') echo shift-apostrophe ;;
        '[') echo bracket_left ;; '{') echo shift-bracket_left ;;
        ']') echo bracket_right ;; '}') echo shift-bracket_right ;;
        '\') echo backslash ;;    '|') echo shift-backslash ;;
        '`') echo grave_accent ;; '~') echo shift-grave_accent ;;
        '!') echo shift-1 ;; '@') echo shift-2 ;; '#') echo shift-3 ;; '$') echo shift-4 ;;
        '%') echo shift-5 ;; '^') echo shift-6 ;; '&') echo shift-7 ;; '*') echo shift-8 ;;
        '(') echo shift-9 ;; ')') echo shift-0 ;;
        *) die "no key for character: $1" ;;
    esac
}

keys_for() {
    s=$1
    while [ -n "$s" ]; do
        rest=${s#?}
        c=${s%"$rest"}
        echo "sendkey $(qcode "$c")"
        s=$rest
    done
}

monitor_cmds() {
    enter=0
    while [ $# -gt 0 ]; do
        case "$1" in
            -n) enter=1 ;;
            -k) shift; echo "sendkey $1" ;;
            *)  keys_for "$1"
                if [ "$enter" = 1 ]; then echo "sendkey ret"; fi ;;
        esac
        shift
    done
}

# A function, not an inline loop: macOS /bin/sh (bash 3.2) misparses case inside $( ).
cmds=$(monitor_cmds "$@")
[ -n "$cmds" ] || die "nothing to type"
# Send in chunks: the guest's USB keyboard drops keys when they arrive faster than it polls.
printf '%s\n' "$cmds" | ssh "$RETRO" "d=\$(mktemp -d) && cd \$d && split -l 20 - c. &&
    for f in c.*; do qm monitor $VMID < \$f >/dev/null 2>&1; sleep 0.3; done; rm -rf \$d"
