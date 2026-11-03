#!/bin/bash
# TEST: DIFFERENCES, SORT, CONVERT and DUMP reach files only through RMS; a file RMS cannot open is reported with the RMS reason (libc-drop vms-0504)
# EXPECT: contains:%DIFF-F-OPENIN, error opening SYS$SYSDEVICE:[
# EXPECT: contains:NOSUCH_A.TXT; as input
# EXPECT: contains:%SORT-F-OPENIN, error opening NOSUCH_B.TXT as input
# EXPECT: contains:%CONVERT-E-OPENIN, error opening NOSUCH_C.TXT as input
# EXPECT: contains:-RMS-E-FNF, file not found
# EXPECT: contains:SORT_VIA_RMS_OK
# EXPECT: contains:Number of difference sections found: 0
# EXPECT: contains:DUMP_VIA_RMS_OK
# EXPECT_NOT: contains:SORT_VIA_RMS_BAD
# EXPECT_NOT: contains:DUMP_VIA_RMS_BAD
#
# Negative control for the libc drop: each verb's failure comes from RMS's own
# status (OPENIN over -RMS-E-FNF), not a host fopen()'s errno. SORT's output
# is a new version RMS $CREATEd (SORTED.TXT;1, variable-length records on the
# host tree), which DIFFERENCES, TYPE and DUMP then reach by its VMS name
# through RMS -- a host stdio read of it would see record-length words, not
# lines, and DIFFERENCES would not find it identical to EXPECT.TXT.
VMSDCL="${VMSDCL:-vmsdcl}"
TNAME="dcl_mrms_$$"
TDIR="/vms/$TNAME"
VDIR="$(echo "$TNAME" | tr a-z A-Z)"
mkdir -p "$TDIR"
trap 'rm -rf "$TDIR"' EXIT
printf 'charlie\nalpha\nbravo\n' > "$TDIR/in.txt"
printf 'alpha\nbravo\ncharlie\n' > "$TDIR/expect.txt"

printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nDIFFERENCES NOSUCH_A.TXT IN.TXT\nSORT NOSUCH_B.TXT OUT.TXT\nCONVERT NOSUCH_C.TXT OUT.TXT\nSORT IN.TXT SORTED.TXT\nDIFFERENCES SORTED.TXT EXPECT.TXT\n' "$VDIR" | $VMSDCL 2>&1

# SORTED.TXT;1 is an RMS variable-length-record file; TYPE reads it back
# through RMS $GET in sorted order.
seq=$(printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nTYPE SORTED.TXT\n' "$VDIR" | $VMSDCL 2>&1 | grep -oE '^(alpha|bravo|charlie)$' | tr '\n' ,)
if [ -f "$TDIR/sorted.txt;1" ] && [ "$seq" = "alpha,bravo,charlie," ]; then
    echo SORT_VIA_RMS_OK
else
    echo "SORT_VIA_RMS_BAD: seq=$seq files=$(ls "$TDIR" | tr '\n' ' ')"
fi

d=$(printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nDUMP SORTED.TXT\n' "$VDIR" | $VMSDCL 2>&1)
# The record bytes show in the dump's ASCII column.
if echo "$d" | grep -q 'alpha'; then
    echo DUMP_VIA_RMS_OK
else
    echo "DUMP_VIA_RMS_BAD: $d"
fi
