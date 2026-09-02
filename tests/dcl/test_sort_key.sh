#!/bin/bash
# TEST: SORT /KEY=(POSITION:n,SIZE:m) sorts on the FIELD, not the whole line
#       (vms-e76). The input is chosen so the whole-line order (by the first
#       column) differs from the keyed order (by the digit at column 5), so the
#       assertion discriminates a real field sort from the old whole-line-only
#       qsort that silently ignored /KEY.
#
#       Teeth: before the fix /KEY was accepted but ignored, so the keyed sort
#       produced the whole-line order -> KSEQ=aaa,mmm,zzz -> SORT_KEY_ORDER_BAD.
#
# EXPECT: contains:SORT_KEY_ORDER_OK
# EXPECT: contains:SORT_WHOLE_ORDER_OK
# EXPECT_NOT: contains:SORT_KEY_ORDER_BAD
VMSDCL="${VMSDCL:-vmsdcl}"

TDIR="dcl_sort_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$VDIR"

# Whole-line order (first column): aaa < mmm < zzz.
# Field order (the digit at column 5): 1(zzz) < 2(mmm) < 3(aaa).
printf 'zzz 1\naaa 3\nmmm 2\n' > "/vms/$VDIR/sortin.txt"

# --- keyed sort on the [POSITION:5,SIZE:1] field -> zzz,mmm,aaa ---
KOUT=$(printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nSORT/KEY=(POSITION:5,SIZE:1) SORTIN.TXT KSORT.TXT\nTYPE KSORT.TXT\n' "$VDIR" | $VMSDCL 2>&1)
KSEQ=$(echo "$KOUT" | grep -oE 'zzz|aaa|mmm' | tr '\n' ',')
if [ "$KSEQ" = "zzz,mmm,aaa," ]; then
    echo "SORT_KEY_ORDER_OK"
else
    echo "SORT_KEY_ORDER_BAD:$KSEQ"
fi

# --- whole-line sort (no /KEY) -> aaa,mmm,zzz (unchanged legacy behaviour) ---
WOUT=$(printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nSORT SORTIN.TXT WSORT.TXT\nTYPE WSORT.TXT\n' "$VDIR" | $VMSDCL 2>&1)
WSEQ=$(echo "$WOUT" | grep -oE 'zzz|aaa|mmm' | tr '\n' ',')
if [ "$WSEQ" = "aaa,mmm,zzz," ]; then
    echo "SORT_WHOLE_ORDER_OK"
else
    echo "SORT_WHOLE_ORDER_BAD:$WSEQ"
fi

rm -rf "/vms/$VDIR"
