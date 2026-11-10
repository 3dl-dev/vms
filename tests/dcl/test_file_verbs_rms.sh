#!/bin/bash
# TEST: SEARCH, APPEND and PURGE reach files only through RMS; a file or directory RMS cannot reach is reported with the RMS reason (libc-drop vms-2cbe)
# EXPECT: contains:%SEARCH-W-OPENIN, error opening NOSUCH_S.TXT as input
# EXPECT: contains:%APPEND-E-OPENIN, error opening NOSUCH_A.TXT as input
# EXPECT: contains:-RMS-E-FNF, file not found
# EXPECT: contains:%PURGE-W-SEARCHFAIL, error searching for [NOSUCH_DIR_Q]
# EXPECT: contains:-RMS-E-DNF, directory not found
# EXPECT: contains:APPEND_VIA_RMS_OK
# EXPECT: contains:SEARCH_HIT_IN_RMS_FILE
# EXPECT: contains:PURGE_VIA_RMS_OK
# EXPECT_NOT: contains:APPEND_VIA_RMS_BAD
# EXPECT_NOT: contains:PURGE_VIA_RMS_BAD
#
# Negative control for the libc drop. Each failure comes from RMS's own status
# (OPENIN over -RMS-E-FNF; PURGE's $SEARCH over -RMS-E-DNF), not a host
# errno. LOG.TXT;1 is a variable-length-record file RMS $CREATEd (CREATE),
# so its on-disk bytes are record-length words, not lines: APPEND must $GET
# the source's records and $PUT them at LOG.TXT's end, and SEARCH must $GET
# LOG.TXT's records -- a host stdio byte copy or fgets() would get neither
# right. PURGE finds versions by $SEARCH of NAME.TYP;* and removes them by
# $ERASE.
VMSDCL="${VMSDCL:-vmsdcl}"
TNAME="dcl_frms_$$"
TDIR="/vms/$TNAME"
VDIR="$(echo "$TNAME" | tr a-z A-Z)"
mkdir -p "$TDIR"
trap 'rm -rf "$TDIR"' EXIT
printf 'second-line\nthird-line\n' > "$TDIR/more.txt"
for v in 1 2 3; do echo "v$v" > "$TDIR/pk.txt;$v"; done
cat > "$TDIR/mk.com" <<'EOF'
$ CREATE LOG.TXT
$ DECK
first-line
$ EOD
EOF

# @MK.COM ends its session: CREATE's in-stream data is the procedure's DECK.
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n@MK.COM\n' "$VDIR" | $VMSDCL 2>&1
out=$(printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nSEARCH NOSUCH_S.TXT "x"\nAPPEND NOSUCH_A.TXT LOG.TXT\nPURGE [NOSUCH_DIR_Q]\nAPPEND MORE.TXT LOG.TXT\nTYPE LOG.TXT\nSEARCH LOG.TXT "third"\nPURGE PK.TXT\n' "$VDIR" | $VMSDCL 2>&1)
echo "$out"

seq=$(echo "$out" | grep -oE '^(first|second|third)-line$' | head -3 | tr '\n' ,)
if [ "$seq" = "first-line,second-line,third-line," ]; then
    echo APPEND_VIA_RMS_OK
else
    echo "APPEND_VIA_RMS_BAD: $seq"
fi
# SEARCH's hit is the record after TYPE's three lines.
if [ "$(echo "$out" | grep -c '^third-line$')" -ge 2 ]; then
    echo SEARCH_HIT_IN_RMS_FILE
fi
if [ -e "$TDIR/pk.txt;3" ] && [ ! -e "$TDIR/pk.txt;2" ] && [ ! -e "$TDIR/pk.txt;1" ]; then
    echo PURGE_VIA_RMS_OK
else
    echo "PURGE_VIA_RMS_BAD: $(ls "$TDIR" | tr '\n' ' ')"
fi
