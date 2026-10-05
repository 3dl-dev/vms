#!/bin/bash
# run_pt_null_elim.sh -- the alpha-dec-vms port compiler eliminates the soft
# frame pointer when reload turns a PT_NULL procedure into a stack procedure
# (vms-fd1, patches/0006). Runs INSIDE the ovmx-cross-alpha-vms image:
#   docker run --rm -v <repo>/tools/cross-alpha-vms/test:/t:ro ovmx-cross-alpha-vms bash /t/run_pt_null_elim.sh
# For each option set: compile the fixture, require no `($63)` operand, require
# it to assemble, and require every reload slot ($29/$30-based store feeding an
# ldt) to lie inside the declared .frame. Unpatched, -O2 also spills
# ordinary registers to `0($63)`. Unpatched, -O1 emits `stq ..,0($63)`.
set -euo pipefail
CC=${CC:-/opt/cross-alpha-vms/bin/alpha-dec-vms-gcc}
AS=${AS:-/opt/cross-alpha-vms/bin/alpha-dec-vms-as}
T=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
fails=0
for opts in "-O1" "-O2" "-O1 -mcpu=ev4" "-O2 -mcpu=ev5"; do
    $CC -mpointer-size=64 -mieee $opts -S "$T/pt_null_secmem.c" -o "$W/t.s"
    n63=$(grep -c '(\$63)' "$W/t.s" || true)
    if [ "$n63" != 0 ]; then
        echo "  FAIL [$opts]: $n63 operand(s) still address the soft frame pointer (\$63):"
        grep -n '(\$63)' "$W/t.s" | head -4 | sed 's/^/      /'
        fails=$((fails+1)); continue
    fi
    if ! $AS "$W/t.s" -o "$W/t.obj" 2>"$W/as.log"; then
        echo "  FAIL [$opts]: does not assemble"; sed 's/^/      /' "$W/as.log" | head -4
        fails=$((fails+1)); continue
    fi
    # every int->FP reload slot (stq X,OFF(BASE) immediately reloaded by ldt
    # $fN,OFF(BASE)) must address the frame through $29/$30 inside .frame.
    if ! awk '
        /\.frame \$[0-9]+,[0-9]+,/ { split($2, f, ","); fsize = f[2] + 0 }
        { line[NR] = $0 }
        END {
            for (i = 1; i < NR; i++)
                if (match(line[i], /stq \$[0-9]+,-?[0-9]+\(\$(29|30)\)/)) {
                    s = substr(line[i], RSTART, RLENGTH); sub(/^stq \$[0-9]+,/, "", s)
                    if (index(line[i+1], "ldt") && index(line[i+1], s)) {
                        seen++; off = s; sub(/\(.*/, "", off)
                        if (off + 0 < 0 || off + 0 >= fsize) bad++
                    }
                }
            exit !(seen >= 1 && bad == 0)
        }' "$W/t.s"
    then
        echo "  FAIL [$opts]: no in-frame int->FP reload slot found"; fails=$((fails+1)); continue
    fi
    echo "  PASS [$opts]: soft frame pointer eliminated, slot inside the frame, assembles"
done
rm -rf "$W"
[ "$fails" -eq 0 ] && echo "ALL PT_NULL ELIMINATION (vms-fd1) CHECKS PASSED"
