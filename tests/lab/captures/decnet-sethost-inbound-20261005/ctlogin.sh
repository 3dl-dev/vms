#!/bin/sh
# ctlogin.sh - LAB STAND-IN for LOGINOUT + DCL behind the OVMX CTERM host
# harness (ctermdrv). NOT OVMX product code and authenticates nobody: the
# product runs the real LOGINOUT.EXE on an executive RTAn:. It exists so the
# wire protocol can be driven by a real VAX: a prompt, a no-echo read, line
# input, output, and an exit that ends the session.
printf '\n\n  OVMX CTERM host (lab stand-in session, node 1.44)\n\nUsername: '
IFS= read -r u || exit 0
stty -echo
printf 'Password: '
IFS= read -r p || exit 0
stty echo
printf '\n\n  Lab stand-in: user %s, password of %d characters received (not checked)\n' "$u" "${#p}"
while :; do
    printf '\n$ '
    IFS= read -r line || exit 0
    up=$(printf '%s' "$line" | tr a-z A-Z)
    case "$up" in
        LO|LOG|LOGO|LOGOU|LOGOUT) printf '  %s  logged out at %s\n' "$u" "$(date '+%d-%b-%Y %H:%M:%S' | tr a-z A-Z)"; exit 0 ;;
        "") ;;
        *) printf '  OVMX host received %d characters: "%s"\n' "${#line}" "$line" ;;
    esac
done
