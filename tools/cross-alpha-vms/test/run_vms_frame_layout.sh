#!/bin/bash
# run_vms_frame_layout.sh -- the alpha-dec-vms port compiler keeps a stack
# procedure's locals below the argument-home area (vms-e839, patches/0010).
# Runs INSIDE the ovmx-cross-alpha-vms image:
#   docker run --rm -v <repo>/tools/cross-alpha-vms/test:/t:ro ovmx-cross-alpha-vms bash /t/run_vms_frame_layout.sh
# At -O0, f (void *a, char *b, ...) spills a and b to the frame (the stores
# the prologue makes after .prologue) and calls
# OTS$HOME_ARGS with R1 = the argument pointer; the routine fills the 7
# quadwords from R1 (argument count, then the 6 argument registers). Every
# spill slot must lie below R1. Unpatched, b's slot was the word at R1 itself
# (the count overwrote it).
set -uo pipefail
CC=${CC:-/opt/cross-alpha-vms/bin/alpha-dec-vms-gcc}
W=$(mktemp -d)
cat > "$W/t.c" <<'C'
#include <stdarg.h>
char *g;
char *f(void *a, char *b, ...)
{
    va_list ap;
    va_start(ap, b);
    int x = va_arg(ap, int);
    va_end(ap);
    g = b;
    return b + x;
}
C
fails=0
for ps in "-mpointer-size=64" ""; do
    "$CC" $ps -O0 -S "$W/t.c" -o "$W/t.s"
    home=$(awk '/lda \$1,[0-9]+\(\$29\)/{l=$0} /jsr \$0,OTS\$HOME_ARGS/{print l; exit}' "$W/t.s" \
           | grep -oE '[0-9]+\(\$29\)' | grep -oE '^[0-9]+')
    slots=$(awk '/\.prologue/{p=1;next} p && /^\$LVM1:/{exit} p' "$W/t.s" \
            | grep -oE 'st[lq] \$[0-9]+,[0-9]+\(\$29\)' | grep -oE ',[0-9]+' | tr -d ',' | sort -n | tr '\n' ' ')
    bad=0
    for s in $slots; do [ "$s" -ge "$home" ] && [ "$s" -lt $((home + 56)) ] && bad=1; done
    if [ -n "$home" ] && [ -n "$slots" ] && [ "$bad" = 0 ]; then
        echo "  PASS (${ps:-32-bit}): named-argument slots [$slots] lie below the home area at $home"
    else
        echo "  FAIL (${ps:-32-bit}): home area at '$home', named-argument slots [$slots] -- overlap"; fails=1
    fi
done
rm -rf "$W"
[ "$fails" -eq 0 ] && echo "ALL VMS FRAME LAYOUT (vms-e839) CHECKS PASSED"
exit $fails
