#!/bin/bash
# TEST: LIBRARY/OBJECT/COMPRESS really rewrites the library to /OUTPUT, and an object
# library never holds reclaimable space (vms-097).
# EXPECT: contains:LIBRARIAN-S-COMPRESSED
# EXPECT: contains:COMPRESS-OUTPUT-OK
# EXPECT: contains:NO-RECLAIMABLE-SPACE-OK
# EXPECT: contains:COMPRESS-TEXT-REFUSED-OK
# EXPECT: contains:VAX-REFUSED-OFF-VAX-OK
# EXPECT_NOT: contains:FAIL
#
# Evidence for the register row: every insert/replace/delete rewrites the whole
# ar container from its member list (olb_write), so after any history of updates
# the file is byte-identical to a fresh library built from the surviving members
# -- there is no dead space for /COMPRESS to reclaim. /COMPRESS itself reads the
# members and writes them to /OUTPUT (or over the input), so the file the
# MMK-corpus kit procedure renames afterwards exists.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_cmp_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
CF="-O2 -ffreestanding -fno-builtin -fno-stack-protector -c"
case "$(uname -m)" in x86_64|amd64) CF="$CF -fcf-protection=none" ;; esac
gcc $CF -x c - -o "/vms/$TDIR/AAA.OBJ" <<<'int aaa_fn(void){return 1;}'
gcc $CF -x c - -o "/vms/$TDIR/BBB.OBJ" <<<'int bbb_fn(void){return 2;}'
gcc $CF -x c - -o "/vms/$TDIR/CCC.OBJ" <<<'int ccc_fn(int x){return x*3+1;}'
rm -f /tmp/dcl_cmp_$$.vax

printf 'SET DEFAULT SYS$SYSDEVICE:[%s]
LIBRARY /CREATE HIST.OLB AAA.OBJ
LIBRARY /REPLACE HIST.OLB BBB.OBJ
LIBRARY /REPLACE HIST.OLB CCC.OBJ
LIBRARY /REPLACE HIST.OLB AAA.OBJ
LIBRARY /REPLACE HIST.OLB BBB.OBJ
LIBRARY /DELETE=BBB HIST.OLB
LIBRARY /CREATE FRESH.OLB AAA.OBJ CCC.OBJ
LIBRARY /OBJECT /COMPRESS /OUTPUT=PACKED.OLB HIST.OLB
LIBRARY /TEXT /COMPRESS HIST.TLB
LIBRARY /OBJECT /VAX /CREATE VAXLIB.OLB AAA.OBJ
' "$VDIR" | $VMSDCL 2>&1 | tee /tmp/dcl_cmp_$$.out

HIST=$(find "/vms/$TDIR" -iname 'HIST.OLB' | head -1)
FRESH=$(find "/vms/$TDIR" -iname 'FRESH.OLB' | head -1)
PACKED=$(find "/vms/$TDIR" -iname 'PACKED.OLB' | head -1)
if [ -n "$PACKED" ] && [ -n "$HIST" ] && cmp -s "$HIST" "$PACKED" && ar t "$PACKED" | grep -qi AAA; then
    echo "COMPRESS-OUTPUT-OK"
else
    echo "FAIL: /COMPRESS /OUTPUT did not produce a copy of the library"
fi
if [ -n "$HIST" ] && [ -n "$FRESH" ] && cmp -s "$HIST" "$FRESH"; then
    echo "NO-RECLAIMABLE-SPACE-OK"
else
    echo "FAIL: library after replace/replace/delete history differs from a fresh library of the same members"
fi
grep -q 'COMPRESS\\' /tmp/dcl_cmp_$$.out && echo "COMPRESS-TEXT-REFUSED-OK" || echo "FAIL: /COMPRESS on a TEXT library was accepted"
case "$(uname -m)" in
vax) echo "VAX-REFUSED-OFF-VAX-OK" ;;
*) grep -q 'VAX\\' /tmp/dcl_cmp_$$.out && echo "VAX-REFUSED-OFF-VAX-OK" || echo "FAIL: /VAX accepted off the VAX rail" ;;
esac
rm -rf "/vms/$TDIR" /tmp/dcl_cmp_$$.out
