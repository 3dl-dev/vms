#!/bin/bash
# TEST: DCL file verbs take a P1 list of any length, as OpenVMS does (rd vms-ba2)
# EXPECT: contains:PLIST-DELETE-12-OK
# EXPECT: contains:PLIST-MAXPARM-NOTHING-DELETED
# EXPECT: contains:%DCL-W-MAXPARM, too many parameters - reenter command with fewer parameters
# EXPECT: contains: \L\
# EXPECT: contains:PLIST-TYPE-ORDER-OK
# EXPECT: contains:PLIST-COPY-CONCAT-OK
# EXPECT: contains:PLIST-APPEND-OK
# EXPECT: contains:PLIST-RENAME-OK
# EXPECT: contains:PLIST-DIRECTORY-ONE-HEADER-OK
# EXPECT: contains:%DCL-W-NOLIST, list of parameter values not allowed - check use of comma (,)
# EXPECT: contains: \B.TXT,C\
#
# Oracle: real OpenVMS VAX V7.3, tests/lab/captures/dcl-plist-20261102 (probe
# numbers below). Before: a comma list past P8 was %DCL-W-MAXPARM, and a
# shorter list acted on its FIRST element only (DELETE A,B deleted A).
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="DCL_PLIST_$$"   # upper case: the host-mode RENAME resolves the VMS name as given
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
T="/vms/$TDIR"
mkdir -p "$T/SUB"
mk() { for f in "$@"; do echo "REC-$f" > "$T/$f.TXT"; done; }
run() { { printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n' "$VDIR"; printf '%s\n' "$@"; } | $VMSDCL 2>&1; }
has() { [ -e "$T/$1" ]; }

# #1: twelve elements, past P8 -- every one deleted
mk A B C D E F G H I J K L
run 'DELETE A.TXT;*,B.TXT;*,C.TXT;*,D.TXT;*,E.TXT;*,F.TXT;*,G.TXT;*,H.TXT;*,I.TXT;*,J.TXT;*,K.TXT;*,L.TXT;*'
[ -z "$(ls "$T" | grep -i '\.TXT')" ] && echo PLIST-DELETE-12-OK || echo "PLIST-DELETE-12-FAIL: $(ls "$T")"

# #2: two parameters to a one-parameter verb: MAXPARM \L\, nothing deleted
mk K L
run 'DELETE K.TXT;* L.TXT;*'
has K.TXT && has L.TXT && echo PLIST-MAXPARM-NOTHING-DELETED

# #3: TYPE a list -- in order
mk A B C
OUT=$(run 'TYPE A.TXT,B.TXT,C.TXT')
echo "$OUT" | tr '\n' ' ' | grep -q 'REC-A .*B\.TXT.*REC-B .*C\.TXT.*REC-C' && echo PLIST-TYPE-ORDER-OK

# #4: COPY a list to one file concatenates into it
OUT=$(run 'COPY A.TXT,B.TXT C2.TXT' 'TYPE C2.TXT')
[ "$(cat "$T/C2.TXT" 2>/dev/null | tr '\n' ' ')" = "REC-A REC-B " ] && echo PLIST-COPY-CONCAT-OK

# #6: APPEND a list
run 'APPEND A.TXT,B.TXT C.TXT' >/dev/null
[ "$(cat "$T/C.TXT" | tr '\n' ' ')" = "REC-C REC-A REC-B " ] && echo PLIST-APPEND-OK

# #9: RENAME a list
mk R1 R2
run 'RENAME R1.TXT,R2.TXT *.NEW' >/dev/null
has R1.NEW && has R2.NEW && ! has R1.TXT && ! has R2.TXT && echo PLIST-RENAME-OK

# (PURGE #8 and the sticky directory #10 need the real runtime's version and
# relative-directory handling: tests/qemu/test_syssvc_dcl_plist.c proves them.)

# #7: DIRECTORY a list: one header, one total
mk D E F
OUT=$(run 'DIRECTORY D.TXT,E.TXT,F.TXT')
[ "$(echo "$OUT" | grep -c '^Directory ')" = 1 ] && echo "$OUT" | grep -q 'Total of 3 files\.' &&
    echo PLIST-DIRECTORY-ONE-HEADER-OK || echo "PLIST-DIRECTORY-FAIL: $OUT"

# #15: a list in P2 is NOLIST
run 'COPY A.TXT B.TXT,C.TXT'
rm -rf "$T"
